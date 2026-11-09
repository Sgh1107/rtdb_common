#!/usr/bin/env bash
# ============================================================================
# chaos_m4.sh — M4 验收混沌驱动（docs/16 §6 / docs/05 §5 第 2 项）
#
# 随机 kill 写进程 × N 轮（默认 100）：复用 rtdb_unit_tests 的 NetChaos 用例族。
# 每个用例进程内拉起真实 rtdbd，硬杀服务端进程后重启，并校验：
#   NetChaos.Kill9RestartResumesDelivery
#       kill 后重启续传：无缺漏、交付 seq 严格递增、写者心跳前进
#   NetChaos.KillDuringWriteBurstSustainsDelivery
#       写风暴中杀进程：所有已应答（acked）写入最终必达
# 每轮由 gtest_repeat 独立取回端口与映像（用例内部负责清理），因此 100 轮
# 等价于 100 组独立的「写进程被杀 → 重启 → 数据一致 + 订阅游标连续」验证。
#
# 用法：
#   scripts/chaos_m4.sh [轮数]                    # 默认 100
#   RTDB_TEST_BIN=<path> scripts/chaos_m4.sh 100
#   RTDB_CHAOS_WORKDIR=<dir> scripts/chaos_m4.sh
#
# 退出码：0 = 全部轮次通过；1 = 存在失败；2 = 环境/用法错误。
# 证据：完整输出写入 $RTDB_CHAOS_WORKDIR/chaos_m4_<时间戳>.log。
#
# 口径说明：M1 的 chaos_m1.sh 是引擎级 failpoint 注入（100 轮），M3 的
# chaos_m3.sh 是网络混沌 20 轮冒烟，本脚本是 M4 验收口径的网络混沌 100 轮。
# 三者互补，不得互相顶替。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

BIN="${RTDB_TEST_BIN:-}"
if [ -z "$BIN" ]; then
  for cand in \
      "$ROOT/build/Release/rtdb_unit_tests.exe" \
      "$ROOT/build/Release/rtdb_unit_tests" \
      "$ROOT/build/rtdb_unit_tests.exe" \
      "$ROOT/build/rtdb_unit_tests" \
      "$ROOT/build_wal/Release/rtdb_unit_tests.exe"; do
    if [ -x "$cand" ]; then
      BIN="$cand"
      break
    fi
  done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "chaos_m4: 找不到 rtdb_unit_tests（先构建，或用 RTDB_TEST_BIN 指定）" >&2
  exit 2
fi

ROUNDS="${1:-100}"
case "$ROUNDS" in
  ''|*[!0-9]*)
    echo "chaos_m4: 轮数须为正整数（收到 '$ROUNDS'）" >&2
    exit 2
    ;;
esac
if [ "$ROUNDS" -lt 1 ]; then
  echo "chaos_m4: 轮数须 ≥ 1" >&2
  exit 2
fi

WORKDIR="${RTDB_CHAOS_WORKDIR:-$ROOT/build}"
case "$WORKDIR" in
  /*|?:/*) ;;  # POSIX 绝对路径 / Windows 盘符绝对路径
  *) WORKDIR="$(pwd)/$WORKDIR" ;;
esac
mkdir -p "$WORKDIR"
LOG="$WORKDIR/chaos_m4_$(date +%Y%m%d_%H%M%S).log"

echo "chaos_m4: bin=$BIN rounds=$ROUNDS log=$LOG"
# 在二进制所在目录运行：用例使用相对映像名；异常中断时的残留落在构建产物旁。
cd "$(dirname "$BIN")" || exit 2
"$BIN" --gtest_filter='NetChaos.*' --gtest_repeat="$ROUNDS" --gtest_break_on_failure \
  >"$LOG" 2>&1
rc=$?

pass=$(grep -c "\[       OK \]" "$LOG" 2>/dev/null || true)
fail=$(grep -c "\[  FAILED  \]" "$LOG" 2>/dev/null || true)
echo "chaos_m4: 用例通过 ${pass:-0}、失败 ${fail:-0}（完整输出：$LOG）"
tail -n 6 "$LOG"
# 每轮应恰好跑 2 个 NetChaos 用例；数量不符说明过滤器或构建版本不对。
expected=$((ROUNDS * 2))
if [ "${fail:-0}" -ne 0 ] || [ "${pass:-0}" -ne "$expected" ]; then
  echo "chaos_m4: 用例结果异常（通过 ${pass:-0}/预期 $expected，失败 ${fail:-0}，退出码 $rc）" >&2
  exit 1
fi
# 本机 MinGW 工具链在测试全部 PASSED 之后、进程退出清理阶段偶发 SEGFAULT(139)，
# 属已知环境现象（CI 的 MSVC/GCC 作业无此问题，见 docs/16 §13.3）。判定以
# gtest 用例结果为准；其它非零退出码仍一律视为失败，不掩盖真实问题。
if [ "$rc" -ne 0 ] && [ "$rc" -ne 139 ]; then
  echo "chaos_m4: 进程异常退出（退出码 $rc）" >&2
  exit 1
fi
if [ "$rc" -eq 139 ]; then
  echo "chaos_m4: 注意：退出期 SEGFAULT(139) 为本机 MinGW 工具链现象，用例结果全过" | tee -a "$LOG"
fi
echo "chaos_m4: 全部 $ROUNDS 轮通过（$expected 个用例，无缺漏 / acked 必达）"
exit 0
