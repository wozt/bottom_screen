#!/bin/sh
# Builds an emulator with the changes on it, from upstream rather than
# from the fork.
#
# "The recipe applies" and "the result compiles" are different claims,
# and only the second one is worth anything: an anchor can land in the
# right function and still leave the file wrong, and nothing but a
# compiler will say so. This takes the newest upstream available, applies
# the recipe, and builds it.
#
# Not part of `make test`: it needs a full emulator toolchain and takes
# minutes, so it is run deliberately.
#
#     tests/run_recipe_build.sh melonDS
#     tests/run_recipe_build.sh azahar
#
# The build tree goes on disk rather than in /dev/shm -- an emulator's
# object files are gigabytes, and putting those in RAM is how a machine
# starts swapping.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAME=${1:-melonDS}
BUILD=${BS_BUILD_DIR:-/var/tmp/bs_recipe_build}

case "$NAME" in
    melonDS|azahar) BRANCH=master ;;
    Cemu)           BRANCH=main ;;
    *) echo "unknown emulator: $NAME"; exit 2 ;;
esac

repo="$DIR/emulators/$NAME"
[ -d "$repo/.git" ] || { echo "SKIP BUILD (no $NAME checkout)"; exit 0; }
command -v cmake >/dev/null || { echo "SKIP BUILD (no cmake)"; exit 0; }

tree="$BUILD/$NAME"
cleanup() {
    git -C "$repo" worktree remove --force "$tree" 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$BUILD"
mkdir -p "$BUILD"

ref=""
for candidate in "upstream/$BRANCH" "origin/$BRANCH"; do
    git -C "$repo" rev-parse --verify -q "$candidate" >/dev/null && { ref=$candidate; break; }
done
[ -n "$ref" ] || { echo "SKIP BUILD (no upstream branch to build from)"; exit 0; }

base=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['generated_against'])" \
       "$DIR/patches/recipes/$NAME.json")
ahead=$(git -C "$repo" rev-list --count "$base..$ref" 2>/dev/null || echo 0)

echo "building $NAME from $ref, $ahead commit(s) after the recipe was written"
git -C "$repo" worktree add --detach -q "$tree" "$ref"
git -C "$tree" submodule update --init --recursive --depth 1 -q 2>/dev/null || true

python3 "$DIR/tools/bs_patch.py" "$NAME" "$tree" --quiet || {
    echo "FAIL (the recipe did not go on)"
    exit 1
}

cmake -S "$tree" -B "$tree/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBOTTOM_SCREEN_DIR="$DIR" \
      > "$BUILD/configure.log" 2>&1 || {
    echo "FAIL (cmake refused; see $BUILD/configure.log)"
    tail -15 "$BUILD/configure.log" | sed 's/^/    /'
    exit 1
}

if ! cmake --build "$tree/build" -j"$(nproc)" > "$BUILD/build.log" 2>&1; then
    echo "FAIL (the build broke; see $BUILD/build.log)"
    grep -m 10 -E "error:|Error" "$BUILD/build.log" | sed 's/^/    /'
    exit 1
fi

# The bridge has to be in the binary, not merely compiled beside it: a
# build that quietly skipped it would pass everything above.
if grep -q "bottom_screen" "$BUILD/build.log"; then
    echo "PASS ($NAME builds from $ref with the changes on it)"
else
    echo "FAIL (it built, but nothing named bottom_screen was compiled)"
    exit 1
fi
