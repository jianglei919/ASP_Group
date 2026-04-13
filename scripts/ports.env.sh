#!/usr/bin/env bash
# Shared runtime ports for client and all server nodes.

W26_PRIMARY_PORT="${W26_PRIMARY_PORT:-5000}"
W26_MIRROR1_PORT="${W26_MIRROR1_PORT:-5001}"
W26_MIRROR2_PORT="${W26_MIRROR2_PORT:-5002}"

_validate_port() {
    local name="$1"
    local value="$2"
    if ! [[ "$value" =~ ^[0-9]+$ ]] || ((value < 1 || value > 65535)); then
        echo "Error: $name must be an integer in range 1-65535 (got: $value)" >&2
        exit 1
    fi
}

_validate_port "W26_PRIMARY_PORT" "$W26_PRIMARY_PORT"
_validate_port "W26_MIRROR1_PORT" "$W26_MIRROR1_PORT"
_validate_port "W26_MIRROR2_PORT" "$W26_MIRROR2_PORT"

export W26_PRIMARY_PORT
export W26_MIRROR1_PORT
export W26_MIRROR2_PORT
