#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PID_DIR="$ROOT_DIR/.pids"

stop_one() {
    local name="$1"
    local pid_file="$PID_DIR/${name}.pid"

    if [[ ! -f "$pid_file" ]]; then
        echo "$name: no pid file"
        return
    fi

    local pid
    pid="$(cat "$pid_file")"

    if kill -0 "$pid" 2> /dev/null; then
        kill "$pid"
        echo "$name: stopped (pid $pid)"
    else
        echo "$name: process not running (pid $pid)"
    fi

    rm -f "$pid_file"
}

stop_one "w26server"
stop_one "mirror1"
stop_one "mirror2"
