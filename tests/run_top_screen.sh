#!/bin/sh
# Two clients, two screens, at the same time.
#
# The bottom screen is what this project is for; the top one is a second
# encoder running beside it, and the thing worth proving is that the two
# do not become each other. So both are watched at once and each is
# checked for the size only it can have -- a 3DS is 320x240 below and
# 400x240 above, and no amount of scaling turns one into the other.
#
# Then the parts that are easy to get wrong and quiet when they are:
#
#   - a backend with no top screen must say so, and a client asking for
#     one anyway must stay where it is rather than be dropped;
#   - the top screen's encoder must not exist while nobody is watching,
#     because a feature that is switched off should cost nothing;
#   - clients switching screens under each other must not corrupt the
#     send queues. That last one is here because it did: purging a
#     client's queue without its lock freed the packet its own socket
#     was writing from. It took half a minute of switching to catch, and
#     it is the kind of fault that shows up as an unexplained crash
#     weeks later. Run this under AddressSanitizer when touching that
#     code -- plain builds mostly get away with it.
set -e

if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    rm -rf "$OUT"
}
trap cleanup EXIT

fail=0
note() { echo "  $*"; }

# --- both screens at once ---------------------------------------------
"$DIR/bottom_screen_server" --console 3ds --port "${PORT:-5093}" \
    >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
P=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

( "$DIR/tests/smoke_client" --port "$P" --frames 45 >"$OUT/bottom.txt" 2>&1 ) &
BOT=$!
"$DIR/tests/smoke_client" --port "$P" --frames 45 --screen top \
    >"$OUT/top.txt" 2>&1 || true
wait $BOT || true

grep -q "^PASS" "$OUT/bottom.txt" || { note "the bottom client failed"; fail=1; }
grep -q "^PASS" "$OUT/top.txt"    || { note "the top client failed"; fail=1; }

if grep -q "ecran demande 1, recu 400x240" "$OUT/top.txt"; then
    note "the top client got 400x240 while the bottom one got 320x240"
else
    note "the top client did not get the top screen's size:"
    grep -E "ecran demande|handshake" "$OUT/top.txt" | sed 's/^/    /'
    fail=1
fi

grep -q "ecrans disponibles: masque 3" "$OUT/top.txt" || {
    note "the server did not advertise both screens"; fail=1; }

# The two streams have to be genuinely separate encoders, not one being
# switched: the log says so once per screen.
grep -q "encoding the top screen at 400x240" "$OUT/server.log" || {
    note "no separate encoder was built for the top screen"; fail=1; }

# --- and the top encoder goes away when nobody is watching ------------
#
# Both clients have left by now. The bottom screen's encoder is kept on
# purpose -- it is what a new client's parameter sets come from -- but
# the top one has no such excuse.
"$DIR/tests/smoke_client" --port "$P" --frames 20 >"$OUT/again.txt" 2>&1 || true
if [ "$(grep -c "encoding the top screen" "$OUT/server.log")" = "1" ]; then
    note "the top encoder was built once, for the one client that asked"
else
    note "the top encoder was rebuilt with nobody watching"
    fail=1
fi

# --- clients switching under each other -------------------------------
# Waited for by name, never with a bare `wait`: the server is a
# background job of this shell too, and it does not stop on its own.
pids=""
for i in 1 2 3 4; do
    ( "$DIR/tests/smoke_client" --port "$P" --frames 10 >/dev/null 2>&1 ) &
    pids="$pids $!"
    ( "$DIR/tests/smoke_client" --port "$P" --frames 10 --screen top >/dev/null 2>&1 ) &
    pids="$pids $!"
    ( "$DIR/tests/smoke_client" --port "$P" --frames 6 --screen top >/dev/null 2>&1 ) &
    pids="$pids $!"
done
for pid in $pids; do wait "$pid" 2>/dev/null || true; done
if kill -0 "$SERVER_PID" 2>/dev/null; then
    note "the server survived clients arriving and leaving on both screens"
else
    note "the server died while clients moved between screens"
    tail -20 "$OUT/server.log" | sed 's/^/    /'
    fail=1
fi

# --- joining a screen that is already running -------------------------
#
# The size is announced when it changes, which says nothing to somebody
# arriving. With one client already on the top screen, a second one
# switching to it was told nothing at all and kept the size from its
# handshake -- the bottom screen's -- so it decoded a 5:3 picture as
# though it were 4:3. Whether that happened depended on whether anybody
# else was watching, which is what made it look intermittent.
( "$DIR/tests/smoke_client" --port "$P" --frames 400 --screen top \
    >"$OUT/holder.txt" 2>&1 || true ) &
HOLDER=$!
sleep 3
"$DIR/tests/smoke_client" --port "$P" --frames 40 --screen top \
    >"$OUT/joiner.txt" 2>&1 || true
kill $HOLDER 2>/dev/null || true
wait $HOLDER 2>/dev/null || true

if grep -q "ecran demande 1, recu 400x240" "$OUT/joiner.txt"; then
    note "a client joining a stream already running is told its shape"
else
    note "a client joining a running stream was not told its size:"
    grep -E "ecran demande|handshake" "$OUT/joiner.txt" | sed 's/^/    /'
    fail=1
fi

# --- a second viewer does not wait for the next scheduled keyframe ----
#
# Clients may not ask for keyframes: one that is struggling asks
# constantly, which is exactly when the others can least afford it. But
# joining a stream that is already running means waiting for the next
# scheduled one -- half a second on average, a second at worst, of
# nothing or of green depending on the decoder. The server issues one
# itself for that case, and this is the difference: measured without it,
# 494ms and 495ms; with it, 29ms.
( "$DIR/tests/smoke_client" --port "$P" --frames 400 >/dev/null 2>&1 || true ) &
FIRST=$!
sleep 3
"$DIR/tests/smoke_client" --port "$P" --frames 30 >"$OUT/second.txt" 2>&1 || true
kill $FIRST 2>/dev/null || true
wait $FIRST 2>/dev/null || true

# The counts first, because they are what says the setup happened.
#
# The timing below is only meaningful if there really were two viewers:
# with the first one not yet connected, the second is alone on the
# stream, gets its keyframe because the encoder is being built for it,
# and the measurement says nothing about the thing being tested. Judging
# it anyway is how this failed once inside a busy suite and passed every
# time it was run on its own -- a test that fails for a reason it does
# not check is worse than no test.
if grep -q "masque 3, 2 en bas" "$OUT/second.txt"; then
    note "the server counts who is watching what"

    ms=$(grep -oE "premiere image cle apres [0-9]+" "$OUT/second.txt" |
         grep -oE "[0-9]+$")
    if [ -n "$ms" ] && [ "$ms" -lt 200 ]; then
        note "a second viewer got its first picture in ${ms}ms"
    else
        note "a second viewer waited ${ms:-?}ms for a picture, which is a whole GOP"
        fail=1
    fi
else
    note "the first viewer never arrived, so there was no second viewer to time"
    grep "ecrans disponibles" "$OUT/second.txt" | sed 's/^/    /'
fi

# --- and the same thing through a browser's transport -----------------
#
# The page reaches the server over a WebSocket, which is its own framing
# and its own send path. A switch that works for the native client and
# not for the browser is exactly the kind of gap that only shows up when
# somebody opens the page.
if python3 -c "import websockets" 2>/dev/null; then
    if timeout 90 python3 "$DIR/tests/web_client.py" "$P" 40 top \
            >"$OUT/web.txt" 2>&1; then
        note "$(grep 'ecran change' "$OUT/web.txt")"
    else
        note "the browser transport could not change screen:"
        tail -4 "$OUT/web.txt" | sed 's/^/    /'
        fail=1
    fi
else
    note "python websockets missing, browser transport not checked"
fi

kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

# --- a backend with only one screen -----------------------------------
"$DIR/bottom_screen_server" --console ds --no-top --port "${PORT2:-5094}" \
    >"$OUT/single.log" 2>&1 &
SERVER_PID=$!
P2=$("$DIR/tests/wait_port.sh" "$OUT/single.log" "$SERVER_PID") || {
    cat "$OUT/single.log"; exit 1; }

"$DIR/tests/smoke_client" --port "$P2" --frames 30 --screen top \
    >"$OUT/refused.txt" 2>&1 || true
if grep -q "^PASS" "$OUT/refused.txt" &&
   grep -q "le serveur n'offre pas d'ecran du haut" "$OUT/refused.txt"; then
    note "a server without a top screen says so, and keeps the client on the bottom"
else
    note "a client asking for a screen that does not exist was not handled:"
    tail -5 "$OUT/refused.txt" | sed 's/^/    /'
    fail=1
fi

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
