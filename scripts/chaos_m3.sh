#!/usr/bin/env bash
# ============================================================================
# chaos_m3.sh — M3 网络混沌驱动器（docs/13 §5-W5②）
#
# 真 kill -9 注入 × N 轮：复用 rtdb_unit_tests 的 NetChaos 用例族
# （tests/unit/test_chaos_net.cpp，进程内拉起真实 rtdbd 后硬杀/重启/续传
# 校验；每轮独立取回端口与映像），以 --gtest_repeat 复跑：
#   NetChaos.Kill9RestartResumesDelivery
#       kill 后重启续传：无缺漏 + 交付 seq 严格递增 + 写者心跳前进
#   NetChaos.KillDuringWriteBurstSustainsDelivery
#       写风暴中杀进程：所有已应答（acked）写入最终必达
#
# 用法：
#   scripts/chaos_m3.sh [轮数]                  # 默认 20（每轮 2 个用例）
#   RTDB_TEST_BIN=<path> scripts/chaos_m3.sh 50
#   RTDB_CHAOS_WORKDIR=<dir> scripts/chaos_m3.sh
#
# 退出码：0 = 全部轮次通过；1 = 存在失败；2 = 环境/用法错误。
# 证据：完整输出写入 $RTDB_CHAOS_WORKDIR/chaos_m3_<时间戳>.log。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- 定位测试二进制（多配置/多平台候选；RTDB_TEST_BIN 覆盖）----
BIN="${RTDB_TEST_BIN:-}"
if [ -z "$BIN" ]; then
  for cand in \
      "$ROOT/build/Release/rtdb_unit_tests.exe" \
      "$ROOT/build/Release/rtdb_unit_tests" \
      "$ROOT/build/rtdb_unit_tests" \
      "$ROOT/build_wal/Release/rtdb_unit_tests.exe"; do
    if [ -x "$cand" ]; then
      BIN="$cand"
      break
    fi
  done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "chaos_m3: 找不到测试二进制（先构建，或用 RTDB_TEST_BIN 指定）" >&2
  exit 2
fi

ROUNDS="${1:-20}"
case "$ROUNDS" in
  ''|*[!0-9]*)
    echo "chaos_m3: 轮数须为正整数（收到 '$ROUNDS'）" >&2
    exit 2
    ;;
esac
if [ "$ROUNDS" -lt 1 ]; then
  echo "chaos_m3: 轮数须 ≥ 1" >&2
  exit 2
fi

WORKDIR="${RTDB_CHAOS_WORKDIR:-$ROOT/build}"
case "$WORKDIR" in
  /*) ;;
  *) WORKDIR="$(pwd)/$WORKDIR" ;;
esac
mkdir -p "$WORKDIR"
LOG="$WORKDIR/chaos_m3_$(date +%Y%m%d_%H%M%S).log"

echo "chaos_m3: bin=$BIN rounds=$ROUNDS log=$LOG"
# 在二进制所在目录运行：用例使用相对映像名（rtdb_gtest_chaos_*.rtdb），
# 异常中断时的残留物落在构建产物旁而非仓库根。
cd "$(dirname "$BIN")"
"$BIN" --gtest_filter='NetChaos.*' --gtest_repeat="$ROUNDS" --gtest_break_on_failure \
  >"$LOG" 2>&1
rc=$?

pass=$(grep -c "\[       OK \]" "$LOG" 2>/dev/null || true)
fail=$(grep -c "\[  FAILED  \]" "$LOG" 2>/dev/null || true)
echo "chaos_m3: 用例通过 ${pass:-0}、失败 ${fail:-0}（完整输出：$LOG）"
tail -n 5 "$LOG"
if [ "$rc" -ne 0 ] || [ "${fail:-0}" -ne 0 ]; then
  echo "chaos_m3: 存在失败轮次（退出码 $rc）" >&2
  exit 1
fi
echo "chaos_m3: 全部 $ROUNDS 轮通过"
exit 0
