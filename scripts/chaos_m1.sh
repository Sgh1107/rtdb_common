#!/usr/bin/env bash
# ============================================================================
# chaos_m1.sh — M1 混沌矩阵驱动器（docs/06 §3，docs/08 §3-W6-a）
#
# 随机 kill -9 注入 × N 轮（默认 100）：每轮随机选取「注入点 × 场景 ×
# 持久化档位」组合，派生写负载子进程（复用 rtdb_unit_tests 的
# --rtdb-child-chaos 模式 + failpoint 框架，命中即 quick_exit(9) 等价
# kill -9），随后由 --rtdb-verify-chaos 模式重开映像校验：
#   (a) committed_seq 一致   (b) 无半截记录   (c) 数据零丢失（前缀完整）
#   附加：恢复幂等、崩溃后可写、场景 schema 自洽、快照副本可挂载。
#
# 用法：
#   scripts/chaos_m1.sh [轮数]           # 默认 100
#   RTDB_TEST_BIN=<path> scripts/chaos_m1.sh 50
#   RTDB_CHAOS_WORKDIR=<dir> scripts/chaos_m1.sh
#
# 退出码：0 = 全部轮次验证通过；1 = 存在验证失败；2 = 环境/用法错误。
# 证据：每轮明细与汇总写入 $RTDB_CHAOS_WORKDIR/chaos_m1_<时间戳>.log。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- 定位测试二进制（多配置/多平台候选；可用 RTDB_TEST_BIN 覆盖）----
BIN="${RTDB_TEST_BIN:-}"
if [ -z "$BIN" ]; then
  for cand in \
      "$ROOT/build_wal/Release/rtdb_unit_tests.exe" \
      "$ROOT/build_wal/rtdb_unit_tests" \
      "$ROOT/build/Release/rtdb_unit_tests.exe" \
      "$ROOT/build/rtdb_unit_tests"; do
    if [ -x "$cand" ]; then BIN="$cand"; break; fi
  done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "chaos_m1: 找不到测试二进制（rtdb_unit_tests），请先构建或设置 RTDB_TEST_BIN" >&2
  exit 2
fi

ROUNDS="${1:-100}"
case "$ROUNDS" in
  ''|*[!0-9]*) echo "chaos_m1: 轮数须为正整数（当前：$ROUNDS）" >&2; exit 2;;
esac
[ "$ROUNDS" -gt 0 ] || { echo "chaos_m1: 轮数须 > 0" >&2; exit 2; }

# 工作目录：默认 /tmp/rtdb_chaos（纯 POSIX 路径——Windows Git Bash 下
# $TMPDIR 带盘符前缀，混合分隔符路径会触发部分安全 rm 包装器的拒绝）；
# 可用 RTDB_CHAOS_WORKDIR 覆盖。
WORK="${RTDB_CHAOS_WORKDIR:-/tmp/rtdb_chaos}"
mkdir -p "$WORK" || { echo "chaos_m1: 无法创建工作目录 $WORK" >&2; exit 2; }
LOG="$WORK/chaos_m1_$(date +%Y%m%d_%H%M%S).log"

# ---- 注入点 × 场景兼容矩阵（docs/06 §3 五点 + W5 三点）----
WAL_POINTS=(wal.insert.before wal.append.mid wal.apply.mid wal.commit.before)
BATCH_POINTS=(wal.batch.wal_mid wal.batch.mid)
DDL_POINTS=(snapshot.before_copy alter.before_link secidx.before_link)
DURABILITY=(always per_second never)
NOINJ_SCENARIOS=(insert batch snapshot secidx)

rand() { echo $((RANDOM % $1)); }
pick() {  # pick <数组名>
  local -n arr="$1"
  echo "${arr[$(rand ${#arr[@]})]}"
}

hits=0; completed=0; child_err=0
verify_pass=0; verify_fail=0
{
echo "=== chaos_m1 开始 $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
echo "binary=$BIN rounds=$ROUNDS workdir=$WORK"
} >> "$LOG"

overall=0
for ((r = 1; r <= ROUNDS; ++r)); do
  # 约 1/12 轮为无注入对照（完整负载 → 全量数据必须零丢失）；
  # 其余按类别配对：写路径 / 原子批 / DDL 路径。
  roll=$(rand 12)
  if [ "$roll" -eq 0 ]; then
    point="none"
    scenario="$(pick NOINJ_SCENARIOS)"
  else
    case $(rand 3) in
      0) scenario="insert"; point="$(pick WAL_POINTS)";;
      1) scenario="batch";  point="$(pick BATCH_POINTS)";;
      2)
        point="$(pick DDL_POINTS)"
        case "$point" in
          snapshot.before_copy) scenario="snapshot";;
          alter.before_link)    scenario="alter";;
          secidx.before_link)   scenario="secidx";;
        esac
        ;;
    esac
  fi
  dur="$(pick DURABILITY)"

  img="$WORK/rtdb_chaos_${r}.rtdb"
  rm -f "$img" "$img.snap"

  # ---- 子进程：写负载（命中注入点即 quick_exit(9)）----
  if [ "$point" = "none" ]; then
    RTDB_FAILPOINTS="" "$BIN" --rtdb-child-chaos "$img" "$scenario" "$dur" \
        > "$WORK/child_${r}.log" 2>&1
  else
    RTDB_FAILPOINTS="$point=crash" "$BIN" --rtdb-child-chaos "$img" "$scenario" "$dur" \
        > "$WORK/child_${r}.log" 2>&1
  fi
  crc=$?

  # ---- 验证：重开 + 不变量 ----
  "$BIN" --rtdb-verify-chaos "$img" "$scenario" > "$WORK/verify_${r}.log" 2>&1
  vrc=$?

  case "$crc" in
    9)  hits=$((hits + 1)); hit_desc="crash-hit";;
    0)  completed=$((completed + 1)); hit_desc="load-done";;
    *)  child_err=$((child_err + 1)); hit_desc="child-error($crc)"; overall=1;;
  esac
  if [ "$vrc" -eq 0 ]; then
    verify_pass=$((verify_pass + 1))
  else
    verify_fail=$((verify_fail + 1))
    overall=1
    echo "--- round $r 验证失败（point=$point scenario=$scenario dur=$dur child=$crc）---" >> "$LOG"
    cat "$WORK/verify_${r}.log" >> "$LOG"
  fi

  printf 'round=%03d point=%-20s scenario=%-8s dur=%-10s child=%-16s verify=%s\n' \
      "$r" "$point" "$scenario" "$dur" "$hit_desc" \
      "$([ "$vrc" -eq 0 ] && echo PASS || echo FAIL)" >> "$LOG"

  rm -f "$img" "$img.snap"
done

{
echo "=== chaos_m1 汇总 $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
echo "rounds=$ROUNDS injected_hits=$hits load_completed=$completed child_errors=$child_err"
echo "verify_pass=$verify_pass verify_fail=$verify_fail"
if [ "$hits" -eq 0 ]; then
  echo "WARN: 注入命中数为 0——矩阵空转（检查 failpoint 规格与子进程模式）"
fi
if [ "$overall" -eq 0 ]; then
  echo "RESULT: PASS（数据零丢失/无半截记录/committed_seq 一致/恢复幂等/崩溃后可写）"
else
  echo "RESULT: FAIL"
fi
} >> "$LOG"

tail -n 6 "$LOG"
echo "证据日志: $LOG"
exit "$overall"
