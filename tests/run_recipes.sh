#!/bin/sh
# Checks the recipes against the emulator as it is now, not as it was.
#
# The point of carrying these changes as anchors rather than as a fork is
# to put them on whatever version has just come out and take upstream's
# work with it. So the question this asks is "does it still go on today",
# against the newest upstream commit available locally -- not "does it
# reproduce the fork", which would only ever confirm that an old snapshot
# still matches itself.
#
# It also checks the payload is right, and that is a different question:
# a recipe can apply perfectly and insert the wrong thing. Where the
# upstream tip happens to be the commit the recipe was generated against,
# the result is compared against the fork file by file, which is the one
# place the two can be held side by side.
#
# `git fetch upstream` first for a real answer; without it this measures
# whatever was last pulled.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=${TMPDIR:-/dev/shm}/bs_recipes.$$
WORKTREES=""

cleanup() {
    for w in $WORKTREES; do
        repo=${w%%:*}
        path=${w#*:}
        git -C "$DIR/emulators/$repo" worktree remove --force "$path" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK"
fail=0
tested=0

for pair in melonDS:master azahar:master Cemu:main; do
    name=${pair%%:*}
    branch=${pair#*:}
    repo="$DIR/emulators/$name"
    recipe="$DIR/patches/recipes/$name.json"

    if [ ! -d "$repo/.git" ]; then
        echo "$name: no checkout, skipped"
        continue
    fi
    if [ ! -f "$recipe" ]; then
        echo "$name: no recipe"
        fail=1
        continue
    fi

    base=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['generated_against'])" "$recipe")

    # The newest upstream to hand: the project's own branch if this
    # checkout tracks it, else the fork's, else the base itself.
    ref=""
    for candidate in "upstream/$branch" "origin/$branch" "$base"; do
        if git -C "$repo" rev-parse --verify -q "$candidate" >/dev/null; then
            ref=$candidate
            break
        fi
    done
    [ -n "$ref" ] || { echo "$name: nothing to test against, skipped"; continue; }

    ahead=$(git -C "$repo" rev-list --count "$base..$ref" 2>/dev/null || echo 0)
    short=$(git -C "$repo" rev-parse --short "$ref")

    tree="$WORK/$name"
    git -C "$repo" worktree add --detach -q "$tree" "$ref" 2>/dev/null || {
        echo "$name: cannot check out $ref, skipped"
        continue
    }
    WORKTREES="$WORKTREES $name:$tree"

    if python3 "$DIR/tools/bs_patch.py" "$name" "$tree" --quiet; then
        echo "$name: goes on $ref ($short), $ahead commit(s) after the recipe was written"
        tested=$((tested + 1))
    else
        echo "$name: does NOT go on $ref ($short), $ahead commit(s) after the recipe was written"
        fail=1
        continue
    fi

    # Only where the two are comparable: at the base, the result should
    # be the fork, file for file. Further on it should not be, because
    # upstream has moved -- and that is the point.
    if [ "$ahead" = "0" ]; then
        differing=$(python3 - "$recipe" "$tree" "$repo" <<'PY'
import json, subprocess, sys, os
recipe, applied, repo = sys.argv[1], sys.argv[2], sys.argv[3]
r = json.load(open(recipe))
paths = [f["file"] for f in r["new_files"]] + sorted({e["file"] for e in r["edits"]})
bad = []
for p in paths:
    want = subprocess.run(["git", "-C", repo, "show", f"HEAD:{p}"], capture_output=True)
    if want.returncode != 0:
        bad.append(f"{p} (not in the fork)")
        continue
    got = os.path.join(applied, p)
    if not os.path.exists(got):
        bad.append(f"{p} (not produced)")
        continue
    if open(got, "rb").read().rstrip(b"\n") != want.stdout.rstrip(b"\n"):
        bad.append(p)
print("\n".join(bad))
PY
)
        if [ -n "$differing" ]; then
            echo "  but the result is not what the fork contains:"
            echo "$differing" | sed 's/^/    /'
            fail=1
        else
            echo "  and produces exactly what the fork contains"
        fi
    fi
done

if [ "$tested" = 0 ] && [ "$fail" = 0 ]; then
    echo "SKIP RECIPES (no emulator checkouts to test against)"
    exit 0
fi

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
