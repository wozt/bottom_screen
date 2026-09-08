#!/bin/sh
# Shows the recipes surviving a change a diff does not.
#
# The claim is that anchoring on text beats counting lines. A claim like
# that is worth nothing until something is broken on purpose and both
# are asked to cope, so this reindents the code around a hook -- which is
# what a formatter run upstream does, and the most common harmless change
# there is -- and then tries each in turn.
#
# `git apply` compares context lines exactly and refuses. The recipe
# looks for the text again with the whitespace flattened, finds it, says
# so, and puts the change where it belongs.
#
# It also checks the other direction: an anchor whose code has genuinely
# gone must FAIL rather than land somewhere plausible. A patcher that
# always succeeds is worse than one that refuses, because the failure
# moves from the message to the build.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME=melonDS
FILE=src/frontend/qt_sdl/EmuInstanceAudio.cpp
WORK=${TMPDIR:-/dev/shm}/bs_drift.$$

cleanup() {
    git -C "$DIR/emulators/$NAME" worktree remove --force "$WORK/tree" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -d "$DIR/emulators/$NAME/.git" ]; then
    echo "SKIP DRIFT (no $NAME checkout)"
    exit 0
fi

mkdir -p "$WORK"
REF=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['generated_against'])" \
      "$DIR/patches/recipes/$NAME.json")
git -C "$DIR/emulators/$NAME" worktree add --detach -q "$WORK/tree" "$REF"

# --- the drift: every line of the file indented by one more space ------
python3 - "$WORK/tree/$FILE" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding="utf-8", errors="replace").read().splitlines()
out = [(" " + l if l.strip() else l) for l in lines]
open(p, "w", encoding="utf-8").write("\n".join(out) + "\n")
PY

fail=0

# --- what the diff makes of it ----------------------------------------
if (cd "$WORK/tree" && git apply --check "$DIR/patches/$NAME.patch" 2>/dev/null); then
    echo "  unexpected: the diff still applied, so this proves nothing"
    fail=1
else
    echo "  the diff refuses the reindented file, as expected"
fi

# --- and what the recipe makes of it ----------------------------------
out=$(python3 "$DIR/tools/bs_patch.py" "$NAME" "$WORK/tree" 2>&1)
if echo "$out" | grep -q "FAILED\|AMBIGUOUS\|MISSING"; then
    echo "  the recipe did not cope either:"
    echo "$out" | sed 's/^/    /'
    fail=1
elif echo "$out" | grep -q "ok\*"; then
    echo "  the recipe found the code anyway, and said it had to ignore whitespace"
else
    echo "  the recipe applied, though without reporting the looser match"
fi

# The payload has to actually be in the file, not merely reported.
if grep -q "BottomScreen::SubmitAudio" "$WORK/tree/$FILE"; then
    echo "  the change is in the file"
else
    echo "  the change is NOT in the file"
    fail=1
fi

# --- and a hook whose code is really gone must be refused -------------
git -C "$WORK/tree" checkout -q -- "$FILE"
python3 - "$WORK/tree/$FILE" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="replace").read()
# The lines the audio hook anchors on, rewritten: upstream replacing the
# muting logic with a call of its own, and renaming the function around
# it. Nothing an anchor could reasonably still recognise -- which is the
# point, because it must refuse rather than guess.
s = s.replace("audioCallback", "audioCallbackV2")
s = s.replace(
    "    if ((num_in < 1) || inst->audioMutedByWindowFocus || inst->audioMutedToggle || inst->audioMutedByFastForward)",
    "    if (inst->shouldSilence(num_in))")
open(p, "w", encoding="utf-8").write(s)
PY

out=$(python3 "$DIR/tools/bs_patch.py" "$NAME" "$WORK/tree" 2>&1 || true)
if echo "$out" | grep -q "FAILED"; then
    echo "  a hook whose code is gone is refused, and says which:"
    echo "$out" | grep "FAILED" | head -2 | sed 's/^/    /'
    # And the reason has to be the right one: a renamed function is not
    # the same event as a rewritten body, and somebody reading this goes
    # looking in a different place for each.
    if ! echo "$out" | grep -q "is gone from this file"; then
        echo "  but it blamed the wrong thing -- the function was renamed,"
        echo "  and it should say so rather than that the body changed"
        fail=1
    fi
else
    echo "  a hook whose code is gone was NOT refused -- that is the dangerous case"
    fail=1
fi

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
