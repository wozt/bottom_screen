#!/bin/sh
# The machine asking for something a controller cannot give.
#
# A 3DS or a Wii U stops and asks for a name, a message or a Mii, and an
# emulator answers that with a dialog on whatever desktop it is running
# on. Streamed to a phone in another room, that dialog is somewhere
# nobody can see while the game waits for ever -- so the question goes on
# the wire and whoever is watching answers it.
#
# Checked here rather than only in front of a 3DS, because the failure
# is a game that stops for ever and the path has four ends to it: the
# backend asking, every client being shown it, one answer coming back,
# and the rest being told it is over.
set -e

if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 -c "import websockets" 2>/dev/null || {
    echo "SKIP PROMPT (python websockets missing)"; exit 0; }

OUT=$(mktemp -d)
"$DIR/bottom_screen_server" --console 3ds --port "${PORT:-5095}" --prompt \
    >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$OUT"' EXIT

P=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

fail=0
timeout 90 python3 "$DIR/tests/web_client.py" "$P" 200 prompt \
    >"$OUT/client.txt" 2>&1 || true

grep -E "^question" "$OUT/client.txt" | sed 's/^/  /'
grep -q "PASS" "$OUT/client.txt" || {
    echo "  the client did not finish:"
    tail -4 "$OUT/client.txt" | sed 's/^/    /'
    fail=1
}

# Both kinds: text back as text, and a choice back as the index picked.
# The index is the half that goes wrong silently -- a Mii list answered
# with the wrong number is a different Mii, not an error.
if grep -q 'the clients answered: "Robin"' "$OUT/server.log"; then
    echo "  the typed answer reached the backend"
else
    echo "  the typed answer did not arrive:"
    grep -E "answered|declined" "$OUT/server.log" | sed 's/^/    /'
    fail=1
fi
if grep -q "the clients chose 1 (Robin)" "$OUT/server.log"; then
    echo "  the chosen index reached the backend"
else
    echo "  the chosen index did not arrive:"
    grep -E "chose|declined" "$OUT/server.log" | sed 's/^/    /'
    fail=1
fi

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
