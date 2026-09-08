#!/bin/sh
# The console client's network half, against a real server.
#
# It needs no console: switch/source/stream.c has no libnx in it. What it
# does need is opus, which the desktop build does not otherwise use, so
# this skips rather than fails where that is missing.
set -e

if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -x "$DIR/tests/switch_client" ] || { echo "SKIP SWITCH (not built)"; exit 0; }

OUT=$(mktemp -d)
"$DIR/bottom_screen_server" --console 3ds --port "${PORT:-5097}" \
    >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$OUT"' EXIT

P=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

"$DIR/tests/switch_client" "$P"
