#!/bin/sh
# The card's own encoder, where there is one.
#
# Skipped rather than failed on a machine without one -- most are, and a
# test that fails for the absence of hardware says nothing about the
# code. Where there is one, the thing worth checking is not that the
# encoder opens but that a client can decode what comes out of it, and
# that a client arriving late can too.
#
# That second half is the whole bet. This server deliberately does not
# set AV_CODEC_FLAG_GLOBAL_HEADER, so the parameter sets ride in front of
# every keyframe and any client can start at the next one. x264 does that
# because it was asked to; a hardware encoder does it because its driver
# does, which is a different kind of promise and worth measuring rather
# than assuming.
set -e

if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# What this machine actually has, asked of the encoder rather than of
# lspci: a card can be present and its encoder absent, or present and
# refused by the driver.
"$DIR/bottom_screen_server" --console 3ds --port "${PORT:-5089}" \
    --encoder auto >"$OUT/probe.log" 2>&1 &
PROBE=$!
sleep 5
chosen=$(grep -oE "libx264|h264_[a-z0-9]+" "$OUT/probe.log" | head -1)
kill $PROBE 2>/dev/null || true
wait $PROBE 2>/dev/null || true

if [ -z "$chosen" ] || [ "$chosen" = "libx264" ]; then
    echo "SKIP HARDWARE (auto chose ${chosen:-nothing}, so there is none here)"
    exit 0
fi
echo "  auto chose $chosen"

fail=0
"$DIR/bottom_screen_server" --console 3ds --port "${PORT:-5089}" \
    --encoder "$chosen" >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; rm -rf "$OUT"' EXIT

P=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

"$DIR/tests/smoke_client" --port "$P" --frames 60 >"$OUT/first.txt" 2>&1 || true
grep -q "^PASS" "$OUT/first.txt" || {
    echo "  a client could not decode it:"
    tail -3 "$OUT/first.txt" | sed 's/^/    /'
    fail=1
}

# Late, which is where the parameter sets matter: this one missed the
# opening keyframe and everything before it.
sleep 4
"$DIR/tests/smoke_client" --port "$P" --frames 60 >"$OUT/late.txt" 2>&1 || true
if grep -q "^PASS" "$OUT/late.txt"; then
    echo "  a client joining late decodes it too, so the headers repeat"
else
    echo "  a late client could not decode it -- the parameter sets are"
    echo "  not being repeated, and every client that misses the first"
    echo "  keyframe would show nothing:"
    tail -3 "$OUT/late.txt" | sed 's/^/    /'
    fail=1
fi

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
