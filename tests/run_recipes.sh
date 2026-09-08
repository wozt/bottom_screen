#!/bin/sh
# Proves the recipes reproduce the forks, file for file.
#
# A patch that applies is not the same as a patch that produces the right
# result, and an anchored edit has more ways to land in the wrong place
# than a diff does -- it is looking for text rather than counting lines.
# So this does not check that tools/bs_patch.py reports success: it takes
# an untouched copy of upstream, applies the recipe to it, and compares
# every file the recipe claims to touch against what the fork actually
# contains. Anything short of identical is a failure.
#
# Skipped where a fork is not checked out, since there is then nothing to
# compare against.
set -e

DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=${TMPDIR:-/dev/shm}/bs_recipes.$$
trap 'rm -rf "$WORK"; cleanup_worktrees' EXIT

WORKTREES=""
cleanup_worktrees() {
    for w in $WORKTREES; do
        repo=${w%%:*}
        path=${w#*:}
        git -C "$DIR/emulators/$repo" worktree remove --force "$path" 2>/dev/null || true
    done
}

mkdir -p "$WORK"
fail=0
tested=0

for name in melonDS azahar Cemu; do
    repo="$DIR/emulators/$name"

    if [ ! -d "$repo/.git" ]; then
        echo "$name: no checkout, skipped"
        continue
    fi
    if [ ! -f "$DIR/patches/recipes/$name.json" ]; then
        echo "$name: no recipe"
        fail=1
        continue
    fi

    # The commit the recipe was generated against, not whatever the
    # fork's upstream branch points at today: those are 71 commits apart
    # for one of these, and comparing against the wrong base would
    # report upstream's own changes as our failures.
    ref=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['generated_against'])" \
          "$DIR/patches/recipes/$name.json")

    clean="$WORK/$name"
    git -C "$repo" worktree add --detach -q "$clean" "$ref" 2>/dev/null || {
        echo "$name: cannot reach $ref, skipped"
        continue
    }
    WORKTREES="$WORKTREES $name:$clean"

    if ! python3 "$DIR/tools/bs_patch.py" "$name" "$clean" --quiet; then
        echo "$name: the recipe did not apply cleanly to $ref"
        fail=1
        continue
    fi

    # Every file the recipe touches, against what the fork has.
    differing=$(python3 - "$DIR/patches/recipes/$name.json" "$clean" "$repo" <<'PY'
import json, subprocess, sys, os
recipe, applied, repo = sys.argv[1], sys.argv[2], sys.argv[3]
r = json.load(open(recipe))
paths = [f["file"] for f in r["new_files"]] + \
        sorted({e["file"] for e in r["edits"]})
bad = []
for p in paths:
    want = subprocess.run(["git", "-C", repo, "show", f"HEAD:{p}"],
                          capture_output=True)
    if want.returncode != 0:
        bad.append(f"{p} (not in the fork)")
        continue
    got_path = os.path.join(applied, p)
    if not os.path.exists(got_path):
        bad.append(f"{p} (not produced)")
        continue
    got = open(got_path, "rb").read()
    if got.rstrip(b"\n") != want.stdout.rstrip(b"\n"):
        bad.append(p)
print("\n".join(bad))
PY
)

    if [ -n "$differing" ]; then
        echo "$name: the result differs from the fork:"
        echo "$differing" | sed 's/^/    /'
        fail=1
    else
        count=$(python3 -c "import json,sys;r=json.load(open(sys.argv[1]));print(len(r['new_files'])+len(r['edits']))" "$DIR/patches/recipes/$name.json")
        echo "$name: $count change(s) reproduce the fork exactly"
        tested=$((tested + 1))
    fi
done

if [ "$tested" = 0 ] && [ "$fail" = 0 ]; then
    echo "SKIP RECIPES (no emulator checkouts to test against)"
    exit 0
fi

[ "$fail" = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
