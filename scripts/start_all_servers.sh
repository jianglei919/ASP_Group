#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="$ROOT_DIR/logs"
PID_DIR="$ROOT_DIR/.pids"
SEARCH_ROOT=""
MAX_DEPTH=""

usage() {
    cat << 'EOF'
Usage: scripts/start_all_servers.sh [--root <path>] [--depth <1-64>]

Options:
  --root <path>   Override server search root (W26_SEARCH_ROOT)
  --depth <n>     Limit recursion depth (W26_MAX_SCAN_DEPTH), range 1-64
  -h, --help      Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)
            if [[ $# -lt 2 ]]; then
                echo "Error: --root requires a path argument"
                exit 1
            fi
            SEARCH_ROOT="$2"
            shift 2
            ;;
        --depth)
            if [[ $# -lt 2 ]]; then
                echo "Error: --depth requires a numeric argument"
                exit 1
            fi
            MAX_DEPTH="$2"
            shift 2
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

if [[ -n "$SEARCH_ROOT" && ! -d "$SEARCH_ROOT" ]]; then
    echo "Error: --root path is not a directory: $SEARCH_ROOT"
    exit 1
fi

if [[ -n "$MAX_DEPTH" ]]; then
    if ! [[ "$MAX_DEPTH" =~ ^[0-9]+$ ]] || ((MAX_DEPTH < 1 || MAX_DEPTH > 64)); then
        echo "Error: --depth must be an integer in range 1-64"
        exit 1
    fi
fi

mkdir -p "$LOG_DIR" "$PID_DIR"

cd "$ROOT_DIR"

if [[ ! -x "./out/w26server" || ! -x "./out/mirror1" || ! -x "./out/mirror2" ]]; then
    echo "Error: binaries not found. Please run: make"
    exit 1
fi

# 如果已有旧进程在运行则退出；若进程已死则自动清理残留 pid 文件
_has_live=false
for _pidfile in "$PID_DIR"/*.pid; do
    [[ -f "$_pidfile" ]] || continue
    _pid=$(<"$_pidfile")
    if kill -0 "$_pid" 2>/dev/null; then
        _has_live=true
        echo "Error: $(basename "$_pidfile" .pid) (pid $_pid) is still running."
    else
        rm -f "$_pidfile"
    fi
done
if $_has_live; then
    echo "Run scripts/stop_all_servers.sh first."
    exit 1
fi

nohup env W26_SEARCH_ROOT="$SEARCH_ROOT" W26_MAX_SCAN_DEPTH="$MAX_DEPTH" ./out/w26server > "$LOG_DIR/w26server.log" 2>&1 &
echo $! > "$PID_DIR/w26server.pid"

nohup env W26_SEARCH_ROOT="$SEARCH_ROOT" W26_MAX_SCAN_DEPTH="$MAX_DEPTH" ./out/mirror1 > "$LOG_DIR/mirror1.log" 2>&1 &
echo $! > "$PID_DIR/mirror1.pid"

nohup env W26_SEARCH_ROOT="$SEARCH_ROOT" W26_MAX_SCAN_DEPTH="$MAX_DEPTH" ./out/mirror2 > "$LOG_DIR/mirror2.log" 2>&1 &
echo $! > "$PID_DIR/mirror2.pid"

echo "Servers started."
echo "w26server pid: $(cat "$PID_DIR/w26server.pid")"
echo "mirror1   pid: $(cat "$PID_DIR/mirror1.pid")"
echo "mirror2   pid: $(cat "$PID_DIR/mirror2.pid")"
echo "Logs: $LOG_DIR"
if [[ -n "$SEARCH_ROOT" ]]; then
    echo "Search root: $SEARCH_ROOT"
fi
if [[ -n "$MAX_DEPTH" ]]; then
    echo "Max depth: $MAX_DEPTH"
fi
