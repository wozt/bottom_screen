#!/bin/sh
# Starts a server, runs the headless client against it, stops the server.
# Exits non-zero if the pipeline does not carry decodable frames.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORT=${PORT:-5099}
CONSOLE=${CONSOLE:-ds}
FRAMES=${FRAMES:-120}
LOG=$(mktemp)

"$DIR/bottom_screen_server" --console "$CONSOLE" --port "$PORT" >"$LOG" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true; rm -f "$LOG"' EXIT

# Read the port the server actually bound rather than assuming it got the
# one it asked for.
#
# It walks upwards when a port is taken, so on a machine where an earlier
# run left something listening, the port we asked for belongs to a
# stranger. Connecting there anyway produced three convincing passes
# against a server from another session -- a test that cannot fail is
# worse than no test.
PORT=$("$DIR/tests/wait_port.sh" "$LOG" "$SERVER_PID") || { cat "$LOG"; exit 1; }

"$DIR/tests/smoke_client" --port "$PORT" --frames "$FRAMES" || { cat "$LOG"; exit 1; }
