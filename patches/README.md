# The emulator changes

What this project adds to each emulator, on its own, without a fork
around it. There are two ways to carry them, and the second is the one
to reach for when the first stops working.

## A plain patch

```sh
git -C melonDS apply patches/melonDS.patch
```

Each names the upstream commit it was made against, in its header.

A diff says "at line 175, between these three lines". That describes one
snapshot of a file and stops being true the moment anybody edits above
it — and `git apply` then refuses the whole thing, with a message
("patch does not apply") that says nothing about which change failed or
why.

## Or the same changes, anchored

```sh
python3 tools/bs_patch.py melonDS ../melonDS
python3 tools/bs_patch.py melonDS ../melonDS --check   # say, change nothing
```

The recipes in `recipes/` describe the same edits by what they are
rather than where they sit: find this text, inside this function, and
put that in its place. There are no line numbers, so an unrelated change
above a hook costs nothing, and each edit is judged on its own — one
failing does not stop the rest.

When an anchor no longer matches, it says which of the three things
happened:

| | What it means |
|---|---|
| `ok` | the text was found exactly where expected |
| `ok*` | found only after ignoring whitespace — upstream reindented, and the change went in anyway |
| `present` | already applied; nothing to do |
| `FAILED … is gone from this file` | the function this hooks into was renamed or removed |
| `FAILED … is still there, but the lines this hooks into have changed` | the function remains, its body does not |
| `AMBIGUOUS` | the anchor now matches in several places, so it refuses to guess |

The last three are refusals, deliberately. A patcher that always
succeeds is worse than one that stops, because the failure moves from a
message you read to a build you have to debug.

## The point is the version that has just come out

Not the fork. A fork is this project's changes frozen against the
emulator as it was; the reason to carry them as anchors is to put them
on whatever upstream has released since and take its work with them.

Three tests, in the order of how much they claim:

- `tests/run_recipes.sh` applies each recipe to the **newest upstream
  available locally** and reports how far that is from where the recipe
  was written. `git fetch upstream` first for a real answer. Where the
  tip happens to be the commit the recipe was written against, it also
  compares the result against the fork file by file — the payload can be
  wrong even when every anchor lands, and that is the one place the two
  can be held side by side.
- `tests/run_recipe_drift.sh` reindents the code around a hook and
  checks that the diff refuses it, that the recipe copes and says it had
  to, and — the part that matters more — that a hook whose function has
  genuinely gone is refused rather than landed somewhere plausible. Then
  it asks the same of code nobody bent on purpose: Azahar as it stood six
  months earlier, where a real change to the configuration backend moved
  the lines two hooks sit on.
- `tests/run_recipe_build.sh <emulator>` takes upstream, applies the
  recipe and **builds it**. Applying and compiling are different claims:
  an anchor can land in the right function and still leave the file
  wrong, and nothing but a compiler will say so. Not part of `make
  test`; it needs a full toolchain and takes minutes.

Both the patches and the recipes are generated from the forks by
`scripts/make_patches.sh`, so none of the three can drift from the
others — nobody edits any of them by hand.

## Anchors are not magic

They survive more than line numbers do, not everything, and it is worth
being exact about how much more. Walking Azahar's real history backwards
from where the recipes were written, the plain diff and the recipe give
out at the same commit — a change to the configuration backend, 278
commits and six months back, that moved the lines two of the hooks sit
on. Anchoring bought no extra distance there at all.

What it changes is what you are left holding at that point:

| | |
|---|---|
| `git apply` | refuses the whole set, changes nothing, names two files |
| the recipe | lands 20 of the 22 edits, and names the 2 it could not, with the reason for each |

The recipe's failures were `CMakeModules/GenerateSettingKeys.cmake — no
such file upstream` (that file did not exist yet) and `src/common/
settings.h — 'struct Values {' is still there, but the lines this hooks
into have changed`. Recovering from that is an afternoon. Recovering
from "patch does not apply" starts with working out which of
twenty-two changes was the problem.

Where a recipe refuses outright, the fork is the answer: take it, or
rebase its `bottom-screen` branch onto the new upstream. A branch
survives an upstream that keeps moving. That is why all three exist.
