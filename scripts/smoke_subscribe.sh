#!/usr/bin/env bash
# ============================================================================
# smoke_subscribe.sh — M4 rtdbctl 数据面端到端冒烟（docs/05 §5 第 6 项）
#
# 补 ops_drill.sh 未覆盖的 subscribe 子命令链路：
#   起 rtdbd → ddl create → 后台订阅 → import 写入 3 行 → 订阅打印校验
#   （seq 严格递增、insert 事件数、优雅退出）→ rtdbd 自动优雅退出。
#
# 用法：
#   scripts/smoke_subscribe.sh
#   RTDBD_BIN=<path> RTDBCTL_BIN=<path> RTDB_DRILL_WORKDIR=<dir> \
#     RTDB_DRILL_PORT=15600 scripts/smoke_subscribe.sh
#
# 退出码：0 = PASS；1 = 校验失败；2 = 环境/用法错误。
# 证据：$RTDB_DRILL_WORKDIR/smoke_subscribe.log 与 subscribe.out。
# 说明：rtdbd 用 --exit-after-ms 自动优雅退出，不依赖 Windows 控制台信号
#      （与 ops_drill.sh 同一口径）。"坏盘"/崩溃不在本脚本范围内。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

find_bin() {
  local override="$1"; shift
  if [ -n "$override" ]; then echo "$override"; return; fi
  local cand
  for cand in "$@"; do
    if [ -x "$cand" ]; then echo "$cand"; return; fi
  done
  echo ""
}

RTDBD="$(find_bin "${RTDBD_BIN:-}" \
  "$ROOT/build/Release/rtdbd.exe" "$ROOT/build/Release/rtdbd" \
  "$ROOT/build/rtdbd.exe" "$ROOT/build/rtdbd")"
RTDBCTL="$(find_bin "${RTDBCTL_BIN:-}" \
  "$ROOT/build/Release/rtdbctl.exe" "$ROOT/build/Release/rtdbctl" \
  "$ROOT/build/rtdbctl.exe" "$ROOT/build/rtdbctl")"
if [ -z "$RTDBD" ] || [ -z "$RTDBCTL" ]; then
  echo "smoke_subscribe: 找不到 rtdbd / rtdbctl（先构建，或用 RTDBD_BIN/RTDBCTL_BIN 指定）" >&2
  exit 2
fi

WORKDIR="${RTDB_DRILL_WORKDIR:-$ROOT/build/smoke_subscribe_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$WORKDIR" || exit 2
LOG="$WORKDIR/smoke_subscribe.log"
PORT="${RTDB_DRILL_PORT:-15600}"
EXIT_MS="${RTDB_EXIT_AFTER_MS:-25000}"
PW="smoke-pw"
SALT="smoke-salt"
USER="smoke"
IMAGE="smoke_subscribe.rtdb"

run() {
  echo "+ $*" | tee -a "$LOG"
  "$@" >>"$LOG" 2>&1
  local rc=$?
  echo "  rc=$rc" | tee -a "$LOG"
  return $rc
}

echo "smoke_subscribe: rtdbd=$RTDBD rtdbctl=$RTDBCTL workdir=$WORKDIR" | tee "$LOG"
cd "$WORKDIR" || exit 2

HEX="$("$RTDBD" --hash-password "$PW" --salt "$SALT" | tr -d '\r\n')"
if [ ${#HEX} -ne 64 ]; then
  echo "smoke_subscribe: 口令摘要生成失败（长度 ${#HEX}）" | tee -a "$LOG" >&2
  exit 2
fi
cat > smoke.cfg <<CFG
instance = smoke_subscribe
path = $IMAGE
listen = 127.0.0.1:$PORT
size_mb = 64
fsync = per_second
auth_salt = $SALT
user = $USER $HEX
CFG

COMMON=(--host 127.0.0.1 --port "$PORT" --user "$USER" --password "$PW" --salt "$SALT")

"$RTDBD" --config smoke.cfg --exit-after-ms "$EXIT_MS" >>"$LOG" 2>&1 &
PID=$!
ready=0
for i in $(seq 1 40); do
  if "$RTDBCTL" status "${COMMON[@]}" >>"$LOG" 2>&1; then ready=1; break; fi
  sleep 0.25
done
if [ "$ready" -ne 1 ]; then
  echo "smoke_subscribe: rtdbd 未在 10s 内就绪" | tee -a "$LOG" >&2
  exit 2
fi

cat > seed.csv <<'CSV'
id,val,msg
1,10,alpha
2,20,beta
3,30,gamma
CSV

run "$RTDBCTL" ddl create events --col id:int64 --col val:int64 --col msg:utf8 "${COMMON[@]}" || exit 1

# 订阅先起，确认已进入订阅态后再写入。--seconds 限时，避免依赖 Ctrl+C。
"$RTDBCTL" subscribe events --seconds 12 "${COMMON[@]}" > subscribe.out 2>>"$LOG" &
SUB_PID=$!
sleep 2
run "$RTDBCTL" import events --file seed.csv "${COMMON[@]}" || exit 1
wait "$SUB_PID" 2>/dev/null
SUB_RC=$?
echo "smoke_subscribe: subscribe 退出码 $SUB_RC" | tee -a "$LOG"

# ---- 校验：seq 严格递增、insert 事件、覆盖 3 行 ----
EVENTS=$(grep -cE '^#[0-9]+ seq=' subscribe.out 2>/dev/null || true)
INSERTS=$(grep -cE '^#[0-9]+ seq=.*insert' subscribe.out 2>/dev/null || true)
SEQS=$(grep -oE '^#[0-9]+ seq=[0-9]+' subscribe.out 2>/dev/null | grep -oE 'seq=[0-9]+' | cut -d= -f2 || true)
ordered=1
prev=0
for s in $SEQS; do
  if [ "$s" -le "$prev" ]; then ordered=0; break; fi
  prev=$s
done
echo "smoke_subscribe: events=${EVENTS:-0} inserts=${INSERTS:-0} seq_strictly_increasing=$ordered" | tee -a "$LOG"
echo "----- subscribe.out -----" >>"$LOG"
cat subscribe.out >>"$LOG" 2>/dev/null || true

wait "$PID" 2>/dev/null || true
if [ "$SUB_RC" -ne 0 ] || [ "${EVENTS:-0}" -lt 3 ] || [ "${INSERTS:-0}" -lt 3 ] || [ "$ordered" -ne 1 ]; then
  echo "smoke_subscribe: FAIL（订阅未完整交付 3 行 insert 或 seq 非严格递增）" | tee -a "$LOG" >&2
  exit 1
fi
echo "smoke_subscribe: PASS —— 3 行 insert 事件全部交付、seq 严格递增、优雅退出" | tee -a "$LOG"
echo "证据：$LOG / $WORKDIR/subscribe.out"
exit 0
