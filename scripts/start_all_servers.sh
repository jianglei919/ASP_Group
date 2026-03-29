#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="$ROOT_DIR/logs"
PID_DIR="$ROOT_DIR/.pids"

mkdir -p "$LOG_DIR" "$PID_DIR"

cd "$ROOT_DIR"

if [[ ! -x "./out/w26server" || ! -x "./out/mirror1" || ! -x "./out/mirror2" ]]; then
    echo "Error: binaries not found. Please run: make"
    exit 1
fi

# 如果已有旧进程，先提示并退出，避免重复启动
if [[ -f "$PID_DIR/w26server.pid" || -f "$PID_DIR/mirror1.pid" || -f "$PID_DIR/mirror2.pid" ]]; then
    echo "Error: pid files exist. Run scripts/stop_all_servers.sh first."
    exit 1
fi

nohup ./out/w26server > "$LOG_DIR/w26server.log" 2>&1 &
echo $! > "$PID_DIR/w26server.pid"

nohup ./out/mirror1 > "$LOG_DIR/mirror1.log" 2>&1 &
echo $! > "$PID_DIR/mirror1.pid"

nohup ./out/mirror2 > "$LOG_DIR/mirror2.log" 2>&1 &
echo $! > "$PID_DIR/mirror2.pid"

echo "Servers started."
echo "w26server pid: $(cat "$PID_DIR/w26server.pid")"
echo "mirror1   pid: $(cat "$PID_DIR/mirror1.pid")"
echo "mirror2   pid: $(cat "$PID_DIR/mirror2.pid")"
echo "Logs: $LOG_DIR"
