#!/bin/sh
# A console's shape, kept on the way to a GamePad panel.
#
# The panel is 864x480 and no console serves that shape. The bridge used
# to ask the server for 864x480 whatever was on the other end, which
# meant the *server* stretched a 4:3 touch screen a fifth too wide
# before it was ever sent -- and letterboxing in the bridge cannot undo
# that, because the circles are already ovals when they arrive.
#
# So what is checked here is the size asked for, not the size drawn. It
# is the one of the two that has to be right first, and it is the one
# that was wrong.
set -e

if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -x "$DIR/gamepad/bs_gamepad" ] || {
    echo "SKIP GAMEPAD ASPECT (no bridge built; make -C gamepad)"; exit 0; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# A settings file of our own, and the multiplier pinned on the command
# line. This reads what the bridge asks the server for, and the person
# running it has a saved resolution of their own -- which made the test
# report their setting as a fault in the shape.
XDG_CONFIG_HOME="$OUT/config"
export XDG_CONFIG_HOME

fail=0

# console  screen  expected request  the shape it comes from
check() {
    console=$1; screen=$2; want=$3; drawn=$4; note=$5

    "$DIR/bottom_screen_server" --console "$console" --port 0 \
        >"$OUT/server.log" 2>&1 &
    server=$!
    P=$("$DIR/tests/wait_port.sh" "$OUT/server.log" "$server") || {
        cat "$OUT/server.log"; kill $server 2>/dev/null; return 1; }

    timeout 10 "$DIR/gamepad/bs_gamepad" --port "$P" --screen "$screen" \
        --resolution 2 --top-resolution 2 \
        --no-pad >"$OUT/pad.log" 2>&1 || true
    kill $server 2>/dev/null
    wait $server 2>/dev/null || true

    got=$(grep -aoE "asking for [0-9]+x[0-9]+" "$OUT/pad.log" |
          head -1 | cut -d' ' -f3)
    if [ "$got" = "$want" ]; then
        echo "  $console $screen: asks for $got, $note"
    else
        echo "  $console $screen: asks for ${got:-nothing}, wanted $want"
        fail=1
    fi

    # And what is drawn, which is the console's shape fitted to the
    # panel -- its true shape, not the request's. A Wii U screen is
    # 854x480 and is asked for as 848 because 854 is not a whole number
    # of macroblocks; drawing it at 848 would put the rounding into the
    # picture.
    put=$(grep -aoE "drawn as [0-9]+x[0-9]+" "$OUT/pad.log" |
          head -1 | cut -d' ' -f3)
    if [ "$put" != "$drawn" ]; then
        echo "  $console $screen: drawn ${put:-nothing}, wanted $drawn"
        fail=1
    fi
}

# A DS is 4:3 twice and a Wii U 16:9 twice, so only the 3DS tells the two
# screens apart -- which makes its top screen the row that matters, and
# the one that was wrong.
check ds   bottom 640x480 640x480 "which is 4:3 at the panel's height"
check ds   top    640x480 640x480 "the same, because a DS has two of the same"
check 3ds  bottom 640x480 640x480 "which is 4:3 at the panel's height"
check 3ds  top    800x480 800x480 "which is 5:3, and not the 4:3 below it"
check wiiu bottom 848x480 854x480 "which is the GamePad screen's own size"
check wiiu top    848x480 852x480 "16:9, brought down to the panel's height"

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
