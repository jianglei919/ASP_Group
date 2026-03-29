#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PID_DIR="$ROOT_DIR/.pids"

check_one() {
    local name="$1"
    local pid_file="$PID_DIR/${name}.pid"

    if [[ ! -f "$pid_file" ]]; then
        echo "$name: not started"
        return
    fi

    local pid
    pid="$(cat "$pid_file")"

    if kill -0 "$pid" 2> /dev/null; then
        echo "$name: running (pid $pid)"
    else
        echo "$name: stale pid file (pid $pid)"
    fi
}

check_one "w26server"
check_one "mirror1"
check_one "mirror2"
