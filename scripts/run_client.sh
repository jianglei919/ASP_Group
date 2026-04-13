#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

source "$SCRIPT_DIR/ports.env.sh"

cd "$ROOT_DIR"
exec env W26_PRIMARY_PORT="$W26_PRIMARY_PORT" W26_MIRROR1_PORT="$W26_MIRROR1_PORT" W26_MIRROR2_PORT="$W26_MIRROR2_PORT" ./out/client
