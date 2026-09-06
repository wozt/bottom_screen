#!/bin/sh
# Three clients on one server at the same time.
#
# The point is not that three connections are accepted -- it is that they
# are served off one encoder, none of them starves, and the server is
# still standing when they leave one by one. Before this the server took
# a single client and made everyone else wait for it to hang up.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORT=${PORT:-5099}
CONSOLE=${CONSOLE:-ds}
FRAMES=${FRAMES:-90}
CLIENTS=${CLIENTS:-3}
OUT=$(mktemp -d)

"$DIR/bottom_screen_server" --console "$CONSOLE" --port "$PORT" >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true; rm -rf "$OUT"' EXIT

# The port asked for may be taken, in which case the server moves. Ask it
# where it landed instead of guessing -- guessing sent this very test to a
# server left over from another session, which served the three clients
# one after another and still looked like a pass.
PORT=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

i=1
while [ "$i" -le "$CLIENTS" ]; do
    "$DIR/tests/smoke_client" --port "$PORT" --frames "$FRAMES" \
        >"$OUT/client$i.log" 2>&1 &
    eval "PID$i=\$!"
    i=$((i + 1))
done

RC=0
i=1
while [ "$i" -le "$CLIENTS" ]; do
    eval "PID=\$PID$i"
    if wait "$PID"; then
        echo "client $i: $(grep -m1 'decoded' "$OUT/client$i.log" || echo 'aucune image')"
    else
        echo "client $i: ECHEC"
        cat "$OUT/client$i.log"
        RC=1
    fi
    i=$((i + 1))
done

# The server must still be listening once everyone has gone: a client
# leaving used to be able to take the whole thing down with it.
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "le serveur est mort avant ses clients"
    cat "$OUT/server.log"
    exit 1
fi

# One encoder for all of them, so the frame count is shared rather than
# multiplied. A per-client encoder would show up here as N times the work.
echo "--- serveur ---"
grep -E "listening|client|frames" "$OUT/server.log" | tail -8

[ "$RC" -eq 0 ] && echo "PASS MULTICLIENT"
exit "$RC"
