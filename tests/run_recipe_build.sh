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
# The checkout is a real clone rather than a worktree, and it is kept
# between runs. Both for the same reason: a worktree's `.git` is a file,
# and an emulator's build does not expect that -- Azahar copies a
# pre-commit hook into `.git/hooks` and cmake stops on "Not a directory".
# That is the harness failing and blaming the change, which is the one
# thing a test must never do. Keeping the clone also spares refetching
# gigabytes of submodules on every run.
#
# It lives on disk rather than in /dev/shm: an emulator's sources and
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
recipe="$DIR/patches/recipes/$NAME.json"
[ -d "$repo/.git" ] || { echo "SKIP BUILD (no $NAME checkout)"; exit 0; }
[ -f "$recipe" ] || { echo "SKIP BUILD (no recipe for $NAME)"; exit 0; }
command -v cmake >/dev/null || { echo "SKIP BUILD (no cmake)"; exit 0; }

ref=""
for candidate in "upstream/$BRANCH" "origin/$BRANCH"; do
    git -C "$repo" rev-parse --verify -q "$candidate" >/dev/null && { ref=$candidate; break; }
done
[ -n "$ref" ] || { echo "SKIP BUILD (no upstream branch to build from)"; exit 0; }

base=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['generated_against'])" "$recipe")
ahead=$(git -C "$repo" rev-list --count "$base..$ref" 2>/dev/null || echo 0)
sha=$(git -C "$repo" rev-parse "$ref")

src="$BUILD/$NAME/src"
out="$BUILD/$NAME/build"
mkdir -p "$BUILD/$NAME"

echo "building $NAME from $ref, $ahead commit(s) after the recipe was written"

if [ ! -d "$src/.git" ]; then
    # --shared: the objects stay in the checkout next door, so this costs
    # no disk. --no-checkout because the ref comes immediately after.
    git clone --shared --no-checkout -q "$repo" "$src"
fi

# Back to plain upstream. `reset --hard` puts every tracked file back,
# and the recipe's own new files are untracked, so they are named and
# removed rather than reaching for `git clean` -- which would take the
# submodules with it and cost another fetch.
git -C "$src" fetch -q origin 2>/dev/null || true
git -C "$src" checkout -q --detach "$sha"
git -C "$src" reset -q --hard "$sha"
python3 -c "import json,sys;print('\n'.join(f['file'] for f in json.load(open(sys.argv[1]))['new_files']))" \
        "$recipe" | while read -r f; do
    [ -n "$f" ] && rm -f "$src/$f"
done

git -C "$src" submodule update --init --recursive --depth 1 -q 2>/dev/null || true

python3 "$DIR/tools/bs_patch.py" "$NAME" "$src" --quiet || {
    echo "FAIL (the recipe did not go on)"
    exit 1
}

# Wiped every run: a build tree left over from last time could compile
# nothing at all and still be reported as a success.
rm -rf "$out"

cmake -S "$src" -B "$out" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBOTTOM_SCREEN_DIR="$DIR" \
      > "$BUILD/$NAME/configure.log" 2>&1 || {
    echo "FAIL (cmake refused; see $BUILD/$NAME/configure.log)"
    grep -A6 "CMake Error" "$BUILD/$NAME/configure.log" | head -20 | sed 's/^/    /'
    exit 1
}

if ! cmake --build "$out" -j"$(nproc)" > "$BUILD/$NAME/build.log" 2>&1; then
    echo "FAIL (the build broke; see $BUILD/$NAME/build.log)"
    grep -m 10 -E "error:|Error" "$BUILD/$NAME/build.log" | sed 's/^/    /'
    exit 1
fi

# The bridge has to be in the binary, not merely compiled beside it: a
# build that quietly skipped it would pass everything above.
if grep -q "bottom_screen" "$BUILD/$NAME/build.log"; then
    echo "PASS ($NAME builds from $ref with the changes on it)"
else
    echo "FAIL (it built, but nothing named bottom_screen was compiled)"
    exit 1
fi
