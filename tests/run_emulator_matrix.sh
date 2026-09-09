#!/bin/sh
# Every screen of every emulator, one after another, against the real thing.
#
# Six combinations: three consoles, two screens each. For each one it
# starts the emulator, waits for the server it carries to announce a
# port, watches the bottom screen, switches to the top screen, and
# reports what arrived at what size.
#
# **The emulator is restarted between every console.** bs_server.c is
# compiled into each of them, so an emulator left running from before a
# change is the old server wearing the new binary's name -- and it
# produces a pass that means nothing. Restarting is not tidiness here,
# it is the difference between a test and a decoration.
#
# It needs titles to run, and this repository contains none: point it at
# your own through the environment, and anything not given is skipped.
#
#   BS_DS_TITLE=... BS_3DS_TITLE=... BS_WIIU_TITLE=... tests/run_emulator_matrix.sh
#
# With nothing set it falls back to whatever is in games/, which is
# where this machine keeps them and which git ignores.
#
# Not part of `make test`: it wants a display, several minutes and the
# emulators built.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT=${BS_MATRIX_OUT:-/dev/shm/bs_matrix}
FRAMES=${BS_MATRIX_FRAMES:-60}
rm -rf "$OUT"; mkdir -p "$OUT"

fail=0
EMU_PID=""
EMU_PORT=""

# Verified, not assumed, and not by pid alone.
#
# /proc/PID/exe cannot be fooled by a pid that has been reused, but the
# pid this shell started is not always the process that ends up holding
# the port: Cemu re-execs, and Azahar does not stop on the first signal
# at all. Killing only the child left emulators running for hours,
# holding 5090, 5091 and 5092 -- which is how the next test ends up on a
# port it did not expect, talking to a server built before the change it
# is meant to be testing. The port is the thing that matters, so the
# port is what is checked.
stop_emulator() {
    [ -n "$EMU_PID" ] || return 0
    port_held() { ss -ltn 2>/dev/null | grep -q ":$1 "; }

    # Whoever holds the port, plus the child this shell knows about --
    # Cemu re-execs, so the two are not always the same process.
    holder=""
    [ -n "$EMU_PORT" ] && holder=$(ss -ltnp 2>/dev/null | grep ":$EMU_PORT " |
                                   grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
    for pid in $EMU_PID $holder; do kill -TERM "$pid" 2>/dev/null || true; done
    n=0
    while [ $n -lt 15 ]; do
        readlink "/proc/$EMU_PID/exe" >/dev/null 2>&1 || break
        n=$((n + 1)); sleep 1
    done

    if readlink "/proc/$EMU_PID/exe" >/dev/null 2>&1; then
        for pid in $EMU_PID $holder; do kill -9 "$pid" 2>/dev/null || true; done
        sleep 2
    fi

    # Whatever the pids say, nothing may still be listening: that is the
    # only symptom that reaches the next test.
    if [ -n "$EMU_PORT" ] && port_held "$EMU_PORT"; then
        holder=$(ss -ltnp 2>/dev/null | grep ":$EMU_PORT " |
                 grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
        [ -n "$holder" ] && kill -9 "$holder" 2>/dev/null
        sleep 2
    fi
    if [ -n "$EMU_PORT" ] && port_held "$EMU_PORT"; then
        echo "  WARNING: port $EMU_PORT is still held after the emulator was stopped"
        fail=1
    fi
    EMU_PID=""
    EMU_PORT=""
}
trap 'stop_emulator; exit' EXIT INT TERM

# One console: start it, watch both screens, stop it.
run_one() {
    name=$1; bin=$2; title=$3; flag=$4

    if [ ! -x "$bin" ]; then
        echo "$name: not built, skipped"
        return 0
    fi
    if [ -z "$title" ] || [ ! -e "$title" ]; then
        echo "$name: no title to run, skipped"
        return 0
    fi

    echo "$name"
    log="$OUT/$name.log"
    # Cemu wants -g and the executable inside the title, not the folder;
    # the other two take the file itself.
    if [ -n "$flag" ]; then
        "$bin" "$flag" "$title" >"$log" 2>&1 &
    else
        "$bin" "$title" >"$log" 2>&1 &
    fi
    EMU_PID=$!
    #
    # Not run through setsid, however tempting: `setsid cmd &` gives this
    # shell the pid of setsid, which exits immediately, so /proc/PID/exe
    # vanishes at once and every check says the emulator stopped while it
    # is still running. That is worse than not checking, and it is how
    # four of them ended up alive at the same time, holding the ports the
    # next test needed. The port's owner is the authority instead.

    port=""
    n=0
    while [ $n -lt 90 ]; do
        port=$(grep -oE "listening on port [0-9]+" "$log" 2>/dev/null |
               grep -oE "[0-9]+$" | head -1)
        [ -n "$port" ] && { EMU_PORT=$port; break; }
        readlink "/proc/$EMU_PID/exe" >/dev/null 2>&1 || break
        n=$((n + 1)); sleep 1
    done
    if [ -z "$port" ]; then
        echo "  no server announced in 90s"
        tail -3 "$log" | sed 's/^/    /'
        fail=1
        stop_emulator
        return 0
    fi

    # A game's first seconds are its slowest, and the picture is often
    # still black. Waiting is cheaper than reading a black frame and
    # calling it a fault.
    sleep "${BS_MATRIX_WARMUP:-25}"

    for screen in bottom top; do
        arg=""
        [ "$screen" = top ] && arg="--screen top"

        # Tried twice, with a wait between.
        #
        # A game is allowed to draw a black screen, and several do while
        # they load. The client calls a picture with no variation in it a
        # failure -- rightly, because a stream of nothing is usually a
        # backend handing over a cleared buffer -- so it cannot tell the
        # two apart on its own. Half an hour went into a melonDS top
        # screen that was simply black at that moment; the pixels were
        # reaching the encoder correctly the whole time. A second look
        # later is what separates "nothing is being produced" from
        # "nothing is on screen just now".
        "$DIR/tests/smoke_client" --port "$port" --frames "$FRAMES" $arg \
            >"$OUT/$name-$screen.txt" 2>&1 || {
            sleep 20
            "$DIR/tests/smoke_client" --port "$port" --frames "$FRAMES" $arg \
                >"$OUT/$name-$screen.txt" 2>&1 || true
        }
        if grep -q "^PASS" "$OUT/$name-$screen.txt"; then
            size=$(grep -oE "recu [0-9]+x[0-9]+" "$OUT/$name-$screen.txt" | tail -1)
            [ -n "$size" ] || size=$(grep -oE "[0-9]+x[0-9]+ @" "$OUT/$name-$screen.txt" |
                                     head -1 | sed 's/ @//;s/^/recu /')
            luma=$(grep -oE "min [0-9]+, max [0-9]+" "$OUT/$name-$screen.txt" | head -1)
            printf '  %-7s %-18s %s\n' "$screen" "$size" "$luma"
        elif grep -q "the picture is blank" "$OUT/$name-$screen.txt"; then
            # Reported, not failed: the difference between a game
            # drawing black and a backend sending nothing is not
            # visible from here, and calling it a fault sent me
            # looking for one that did not exist.
            printf '  %-7s blank twice -- the game may be on a dark screen\n' "$screen"
        else
            printf '  %-7s FAILED\n' "$screen"
            tail -3 "$OUT/$name-$screen.txt" | sed 's/^/    /'
            fail=1
        fi
    done

    # What the emulator itself said about the second screen, which is
    # where its real size comes from -- the ladder every client offers is
    # measured against it.
    grep -E "the TV picture is|encoding the top screen" "$log" |
        sed 's/^bottom_screen: /  says: /' | head -2

    stop_emulator
}

run_one melonDS "$DIR/emulators/melonDS/build/melonDS" \
    "${BS_DS_TITLE:-$(ls "$DIR"/games/ds/*.nds 2>/dev/null | head -1)}"
# The last one rather than the first: several dumps of the same title can
# sit side by side and only some of them boot, and `ls` order is not a
# statement about which.
run_one azahar  "$DIR/emulators/azahar/build/bin/Release/azahar" \
    "${BS_3DS_TITLE:-$(ls "$DIR"/games/3ds/*.3ds 2>/dev/null | tail -1)}"
run_one Cemu    "$DIR/emulators/Cemu/bin/Cemu_release" \
    "${BS_WIIU_TITLE:-$(find "$DIR"/games/wiiu -name '*.rpx' 2>/dev/null | head -1)}" -g

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
