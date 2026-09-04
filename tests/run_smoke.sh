#!/bin/sh
# Starts a server, runs the headless client against it, stops the server.
# Exits non-zero if the pipeline does not carry decodable frames.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORT=${PORT:-5099}
CONSOLE=${CONSOLE:-ds}
FRAMES=${FRAMES:-120}

"$DIR/bottom_screen_server" --console "$CONSOLE" --port "$PORT" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# smoke_client retries the connection itself, so there is nothing to
# wait for here.
"$DIR/tests/smoke_client" --port "$PORT" --frames "$FRAMES"
