#!/bin/sh
# A client asking for a smaller picture than the emulator renders.
#
# The size is the easy half. The half that goes wrong quietly is touch:
# clients aim in the space the server announced, and if the server hands
# those coordinates to the emulator unconverted, every tap lands short
# by exactly the scale -- which looks like a calibration problem and is
# arithmetic. So the tap is measured, not assumed.
set -e

# Scratch in RAM. /tmp is a tmpfs on most systems but not all, and
# /dev/shm always is; preferring it means a machine where /tmp sits on a
# disk does not take a few megabytes of test output for nothing.
if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi


DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PORT=${PORT:-5092}
OUT=$(mktemp -d)

"$DIR/bottom_screen_server" --console wiiu --port "$PORT" >"$OUT/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true; rm -rf "$OUT"' EXIT

PORT=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$SERVER_PID") || {
    cat "$OUT/server.log"; exit 1; }

# A quarter across and a quarter down, because the centre is the one
# point that looks right whether or not the conversion happened.
"$DIR/tests/smoke_client" --port "$PORT" --frames 90 --ask-size 426x240 \
    --tap 250,250 --hold --dump-yuv "$OUT/frame.yuv" >"$OUT/client.txt" 2>&1
grep -E "demande|decoded" "$OUT/client.txt"

# The size that arrived, not the size asked for.
#
# They are not always the same and the difference is deliberate: H.264
# codes in blocks of sixteen, so the server rounds to a whole number of
# them and says what it settled on. 426 becomes 432. Reading the dump at
# the size that was asked for shears the picture, and a sheared picture
# puts the crosshair somewhere that looks exactly like a touch fault --
# which is what this test exists to catch, so it must not manufacture
# one.
got=$(grep -oE "recu [0-9]+x[0-9]+" "$OUT/client.txt" | tail -1 | cut -d' ' -f2)
[ -n "$got" ] || got=426x240
echo "  the picture arrived at $got"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "ffmpeg missing, cannot check where the tap landed"
    exit 0
fi
ffmpeg -y -v error -f rawvideo -pix_fmt yuv420p -s "$got" \
    -i "$OUT/frame.yuv" "$OUT/frame.png"

python3 - "$OUT/frame.png" <<'PY'
import sys
try:
    from PIL import Image
except ImportError:
    print("PIL missing, cannot check where the tap landed")
    sys.exit(0)

im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
px = im.load()
# The test pattern draws the last touch as a blue crosshair, and nothing
# else in it is that colour.
pts = [(x, y) for y in range(h) for x in range(w)
       if px[x, y][2] > 140 and px[x, y][0] < 90 and px[x, y][1] < 90]
if not pts:
    print("FAILED: no crosshair, so the touch never arrived")
    sys.exit(1)

fx = sum(p[0] for p in pts) / len(pts) / w
fy = sum(p[1] for p in pts) / len(pts) / h
print(f"tap landed at {fx:.3f}, {fy:.3f} (asked for 0.250, 0.250)")
if abs(fx - 0.25) > 0.03 or abs(fy - 0.25) > 0.03:
    print("FAILED: unconverted coordinates would land at 0.125, 0.125")
    sys.exit(1)
print("PASS")
PY
