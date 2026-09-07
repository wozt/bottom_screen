#!/bin/sh
# The patches in patches/ must match the forks they came from.
#
# They are generated, so nobody edits them and they cannot drift that
# way -- but they go stale the moment a fork moves and nobody
# regenerates. That happened: the CMake rework landed in all three forks
# and the patches kept describing the previous shape, applying cleanly
# and silently producing something that no longer built.
#
# "Generated, therefore correct" is only true if something checks.
set -e

# Scratch in RAM. /tmp is a tmpfs on most systems but not all, and
# /dev/shm always is; preferring it means a machine where /tmp sits on a
# disk does not take a few megabytes of test output for nothing.
if [ -z "$TMPDIR" ] && [ -d /dev/shm ]; then
    TMPDIR=/dev/shm
    export TMPDIR
fi


DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp -a "$DIR/patches" "$TMP/before"
"$DIR/scripts/make_patches.sh" >/dev/null

stale=0
for e in melonDS azahar Cemu; do
    [ -f "$TMP/before/$e.patch" ] || continue
    if cmp -s "$TMP/before/$e.patch" "$DIR/patches/$e.patch"; then
        echo "$e.patch is current"
    else
        echo "FAILED: $e.patch was stale -- run scripts/make_patches.sh and commit"
        stale=1
    fi
done

[ "$stale" -eq 0 ] && echo "PASS"
exit "$stale"
