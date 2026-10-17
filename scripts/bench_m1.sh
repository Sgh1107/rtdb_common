#!/usr/bin/env bash
# ============================================================================
# bench_m1.sh — M1 内部 bench 量级确认（docs/05 §4，docs/08 §3-W6-b）
#
# 复用 rtdb_unit_tests 的 --rtdb-bench 模式，输出与 docs/05 §4 基线对标：
#   单行 upsert P99（per_second / always 两档）   基线 ≤ 10μs
#   主键点查 P99                                 基线 ≤ 3μs
#   范围扫描速率                                 基线 10 万行 ≤ 30ms
#   WAL 重放速率                                 基线 1GB 日志 ≤ 60s
# 另跑 --rtdb-bench-block（M2-W2 列式块）：
#   RecordBlock 批量 upsert（8 列混合，512 行块循环） 基线 ≥ 1,000,000 行/s
# 只做量级确认（M1 不设门禁；M2 引入 bench 基线后生效，docs/06 §4）。
#
# 用法：scripts/bench_m1.sh [行数]     # 默认 2000（受行数据区 ≤64KB 约束）
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

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
  echo "bench_m1: 找不到测试二进制（rtdb_unit_tests），请先构建或设置 RTDB_TEST_BIN" >&2
  exit 2
fi

ROWS="${1:-2000}"
WORK="${RTDB_CHAOS_WORKDIR:-/tmp/rtdb_chaos}"
mkdir -p "$WORK"
IMG="$WORK/rtdb_bench.rtdb"
LOG="$WORK/bench_m1_$(date +%Y%m%d_%H%M%S).log"

{
echo "=== bench_m1 $(date -u +%Y-%m-%dT%H:%M:%SZ) binary=$BIN rows=$ROWS ==="
echo "--- per_second 档（热路径无 fsync，对标 P99 ≤ 10μs 基线场景）---"
"$BIN" --rtdb-bench "$IMG" "$ROWS"
echo "--- 列式块 upsert（docs/05 §4 ≥1,000,000 行/s，M2-W2）---"
"$BIN" --rtdb-bench-block "$WORK/rtdb_bench_block.rtdb" 512
} 2>&1 | tee "$LOG" | grep -E "^BENCH|^==="

echo "证据日志: $LOG"
