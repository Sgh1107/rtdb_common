#!/usr/bin/env bash
# ============================================================================
# demo_two_hosts.sh — M3 双机演示脚手架（docs/13 §5-W5③ / docs/14 部署说明）
#
# 两台机器各跑一端（约定用户 demo / 口令 demo123 / 盐 demo-salt / 表 demo，
# 与 remote_demo 默认一致；改约定请直接调 remote_demo 参数）：
#
#   A 机（服务端）：scripts/demo_two_hosts.sh server [--port 5599] [--rows 3] [--write-ms 300]
#   B 机（客户端）：scripts/demo_two_hosts.sh client --host <A 机 IP> [--port 5599] [--seconds 30]
#
# 跨机注意：
#   - A 机防火墙须放行 TCP 端口（Windows: `netsh advfirewall firewall add rule ...`）；
#   - 协议为明文 + 摘要口令（非 TLS），仅限可信内网；生产部署请改盐/口令并
#     限制来源（详见 docs/14 §部署与安全）；
#   - Ctrl+C 结束任一端。
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- 定位 remote_demo（多配置/多平台候选；RTDB_DEMO_BIN 覆盖）----
BIN="${RTDB_DEMO_BIN:-}"
if [ -z "$BIN" ]; then
  for cand in \
      "$ROOT/build/bin/Release/remote_demo.exe" \
      "$ROOT/build/bin/remote_demo.exe" \
      "$ROOT/build/bin/Release/remote_demo" \
      "$ROOT/build/bin/remote_demo" \
      "$ROOT/build/Release/remote_demo.exe"; do
    if [ -x "$cand" ]; then
      BIN="$cand"
      break
    fi
  done
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "demo_two_hosts: 找不到 remote_demo（先构建，或用 RTDB_DEMO_BIN 指定）" >&2
  exit 2
fi

ROLE="${1:-}"
[ $# -gt 0 ] && shift

case "$ROLE" in
  server)
    PORT=5599
    ROWS=3
    WRITE_MS=300
    while [ $# -gt 0 ]; do
      case "$1" in
        --port) PORT="$2"; shift 2 ;;
        --rows) ROWS="$2"; shift 2 ;;
        --write-ms) WRITE_MS="$2"; shift 2 ;;
        *) echo "demo_two_hosts: 未知参数 $1" >&2; exit 2 ;;
      esac
    done
    echo "本机作为服务端：监听 0.0.0.0:$PORT（另一端执行：scripts/demo_two_hosts.sh client --host <本机 IP>）"
    exec "$BIN" --serve --listen "0.0.0.0:$PORT" --rows "$ROWS" --write-ms "$WRITE_MS"
    ;;
  client)
    HOST=""
    PORT=5599
    RUN_SECONDS=30
    while [ $# -gt 0 ]; do
      case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --seconds) RUN_SECONDS="$2"; shift 2 ;;
        *) echo "demo_two_hosts: 未知参数 $1" >&2; exit 2 ;;
      esac
    done
    if [ -z "$HOST" ]; then
      echo "demo_two_hosts: client 模式须给 --host <服务端 IP>" >&2
      exit 2
    fi
    exec "$BIN" --host "$HOST" --port "$PORT" --seconds "$RUN_SECONDS"
    ;;
  *)
    echo "用法：scripts/demo_two_hosts.sh server [--port 5599] [--rows 3] [--write-ms 300]" >&2
    echo "      scripts/demo_two_hosts.sh client --host <服务端 IP> [--port 5599] [--seconds 30]" >&2
    exit 2
    ;;
esac
