#!/usr/bin/env bash
# ============================================================================
# ops_drill.sh — M4 C4 运维手册故障演练（docs/16 §2.2 / docs/05 §5 第 8 项）
#
# 演练链路：建库写数 → 优雅退出抓冷快照 → 备份 → **模拟坏盘**（移走主映像）
#            → 用快照副本恢复 → 重开服务 → 导出比对行数与关键值。
#
# 「模拟坏盘」为**文件级**故障：把主映像移走（不是真实磁盘损坏），用于验证
# 备份/恢复流程与文档命令是否一致；真实坏盘演练需在目标环境另行执行。
#
# 用法：
#   scripts/ops_drill.sh
#   RTDBD_BIN=<path> RTDBCTL_BIN=<path> RTDB_DRILL_WORKDIR=<dir> \
#     RTDB_STOP_CMD='kill -INT $PID' scripts/ops_drill.sh
#
# 依赖：rtdbd / rtdbctl（Release 构建产物）。配置 snapshot_on_exit=true，
# 并用 rtdbd 的 --exit-after-ms 让服务在预算时间后**自动优雅退出**（走
# srv.Stop() → 排空会话 → CreateSnapshot），不依赖 kill -INT 的控制台信号
# 语义（Windows Git Bash 下 kill -INT 对 native 进程无效，实测不会生成 .snap）。
# 可用 RTDB_EXIT_AFTER_MS 调整预算（默认 15000ms；须大于写入+导出耗时）。
#
# 退出码：0 = 演练通过；1 = 校验失败；2 = 环境/用法错误。
# 证据：$RTDB_DRILL_WORKDIR/ops_drill.log（含每步命令、输出与耗时）。
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
  echo "ops_drill: 找不到 rtdbd / rtdbctl（先构建，或用 RTDBD_BIN/RTDBCTL_BIN 指定）" >&2
  exit 2
fi

WORKDIR="${RTDB_DRILL_WORKDIR:-$ROOT/build/ops_drill_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$WORKDIR" || exit 2
LOG="$WORKDIR/ops_drill.log"
PORT="${RTDB_DRILL_PORT:-15599}"
EXIT_MS="${RTDB_EXIT_AFTER_MS:-15000}"
PW="drill-pw"
SALT="drill-salt"
USER="drill"
IMAGE="ops_drill.rtdb"
SNAP="$IMAGE.snap"

run() {
  echo "+ $*" | tee -a "$LOG"
  "$@" >>"$LOG" 2>&1
  local rc=$?
  echo "  rc=$rc" | tee -a "$LOG"
  return $rc
}

echo "ops_drill: rtdbd=$RTDBD rtdbctl=$RTDBCTL workdir=$WORKDIR" | tee "$LOG"
cd "$WORKDIR" || exit 2

# ---- 1) 配置：口令摘要必须与客户端 salt/password 一致 ----
HEX="$("$RTDBD" --hash-password "$PW" --salt "$SALT" | tr -d '\r\n')"
if [ ${#HEX} -ne 64 ]; then
  echo "ops_drill: 口令摘要生成失败（长度 ${#HEX}）" | tee -a "$LOG" >&2
  exit 2
fi
cat > ops_drill.cfg <<CFG
instance = ops_drill
path = $IMAGE
listen = 127.0.0.1:$PORT
size_mb = 64
fsync = per_second
snapshot_on_exit = true
auth_salt = $SALT
user = $USER $HEX
CFG
echo "ops_drill: cfg 已生成（snapshot_on_exit=true）" | tee -a "$LOG"

COMMON=(--host 127.0.0.1 --port "$PORT" --user "$USER" --password "$PW" --salt "$SALT")

start_server() {
  "$RTDBD" --config ops_drill.cfg --exit-after-ms "$EXIT_MS" >>"$LOG" 2>&1 &
  PID=$!
  echo "$PID" > rtdbd.pid
  local i
  for i in $(seq 1 40); do
    if "$RTDBCTL" status "${COMMON[@]}" >>"$LOG" 2>&1; then return 0; fi
    sleep 0.25
  done
  echo "ops_drill: rtdbd 未在 10s 内就绪（见 $LOG）" | tee -a "$LOG" >&2
  return 1
}

stop_server() {
  # --exit-after-ms 到点后 rtdbd 自行退出并抓快照；这里只等它收敛，不发信号，
  # 避免 Windows 控制台信号不触发优雅退出（实测 kill -INT 无效）。
  if [ -n "${RTDB_STOP_CMD:-}" ]; then
    eval "${RTDB_STOP_CMD//\$PID/$PID}" >>"$LOG" 2>&1 || true
  fi
  wait "$PID" 2>/dev/null || true
  echo "ops_drill: rtdbd 已优雅退出（exit-after-ms=$EXIT_MS）" >>"$LOG"
  return 0
}

# ---- 2) 建表 + 导入 3 行 ----
start_server || exit 2
cat > seed.csv <<'CSV'
id,val,name
1,1.5,alpha
2,2.5,beta
3,3.5,gamma
CSV
run "$RTDBCTL" ddl create meters --col id:int64 --col val:float64 --col name:utf8 "${COMMON[@]}" || exit 1
run "$RTDBCTL" import meters --file seed.csv "${COMMON[@]}" || exit 1
run "$RTDBCTL" export meters --file before.csv "${COMMON[@]}" || exit 1
BEFORE=$(wc -l < before.csv | tr -d ' ')
echo "ops_drill: 基线导出行数（含表头）= $BEFORE" | tee -a "$LOG"

# ---- 3) 优雅退出 → 快照；备份 ----
T0=$(date +%s)
stop_server
if [ ! -f "$SNAP" ]; then
  echo "ops_drill: 未生成 $SNAP（自动优雅退出未触发快照）。" | tee -a "$LOG" >&2
  echo "  检查 rtdb_unit_tests 日志、RTDB_EXIT_AFTER_MS 是否足够、映像是否可写。" | tee -a "$LOG" >&2
  echo "  演练判定：FAIL（备份/恢复链路未执行）" | tee -a "$LOG"
  exit 1
fi
mkdir -p backup
cp "$IMAGE" backup/ && cp "$SNAP" backup/
echo "ops_drill: 已备份 $IMAGE / $SNAP" | tee -a "$LOG"

# ---- 4) 模拟坏盘：移走主映像（保留 .snap 作为恢复源）----
mv "$IMAGE" "$IMAGE.lost"
echo "ops_drill: 模拟坏盘 —— 主映像已移走" | tee -a "$LOG"

# ---- 5) 用快照副本恢复并重开验证 ----
cp backup/"$SNAP" "$IMAGE"
start_server || exit 2
run "$RTDBCTL" export meters --file after.csv "${COMMON[@]}" || exit 1
AFTER=$(wc -l < after.csv | tr -d ' ')
echo "ops_drill: 恢复后导出行数（含表头）= $AFTER" | tee -a "$LOG"
stop_server
T1=$(date +%s)

echo "ops_drill: 耗时 $((T1 - T0))s" | tee -a "$LOG"
if [ "$BEFORE" != "$AFTER" ]; then
  echo "ops_drill: FAIL —— 恢复前后行数不一致（$BEFORE vs $AFTER）" | tee -a "$LOG" >&2
  exit 1
fi
if ! diff -q before.csv after.csv >>"$LOG" 2>&1; then
  echo "ops_drill: FAIL —— 恢复前后导出内容不一致" | tee -a "$LOG" >&2
  exit 1
fi
echo "ops_drill: PASS —— 快照备份 → 模拟坏盘 → 恢复后行数与内容一致" | tee -a "$LOG"
echo "证据：$LOG"
exit 0
