#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SEARCH_ROOT=""
MAX_DEPTH=""

source "$SCRIPT_DIR/ports.env.sh"

usage() {
    cat << 'EOF'
Usage: scripts/run_mirror2.sh [--root <path>] [--depth <1-64>]

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

cd "$ROOT_DIR"
exec env W26_SEARCH_ROOT="$SEARCH_ROOT" W26_MAX_SCAN_DEPTH="$MAX_DEPTH" \
    W26_PRIMARY_PORT="$W26_PRIMARY_PORT" W26_MIRROR1_PORT="$W26_MIRROR1_PORT" W26_MIRROR2_PORT="$W26_MIRROR2_PORT" \
    ./out/mirror2
