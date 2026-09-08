#!/usr/bin/env python3
"""Applies this project's emulator changes by what they are, not where.

A unified diff says "at line 175, between these three lines". That is a
description of one snapshot of a file, and it stops being true the moment
anybody adds a line above -- which upstream does constantly. `git apply`
then refuses the whole thing, and the message it gives ("patch does not
apply") says nothing about which change failed or why.

This says the same edits differently: find this text, inside this
function, and put that in its place. Line numbers are gone, so an
unrelated change above the hook costs nothing. What is left is a set of
anchors, and an anchor that no longer matches is a real event worth
reporting precisely -- the function was renamed, or its body changed, or
somebody already applied this.

The recipes are generated from the patches, which are generated from the
forks. Nobody edits either by hand, so neither can drift from what the
forks actually contain: `--generate` rewrites them and the test compares
the result against the fork.
"""

import argparse
import json
import os
import re
import sys


# --------------------------------------------------------------- reading

def parse_patch(text):
    """A patch, as new files plus anchored edits.

    Only the two things a hunk carries that survive time are kept: the
    text on either side of the change, and the function git named in the
    hunk header. The line numbers are dropped deliberately.
    """
    new_files = []
    edits = []

    path = None
    is_new = False
    lines = text.splitlines()
    i = 0

    while i < len(lines):
        line = lines[i]

        if line.startswith("diff --git "):
            m = re.match(r"diff --git a/(.*) b/(.*)", line)
            path = m.group(2) if m else None
            is_new = False
            i += 1
            continue

        if line.startswith("new file mode"):
            is_new = True
            i += 1
            continue

        if line.startswith("@@"):
            m = re.match(r"@@ -\d+(?:,\d+)? \+\d+(?:,\d+)? @@ ?(.*)", line)
            context = (m.group(1).strip() if m else "")
            i += 1

            before, after = [], []
            while i < len(lines):
                l = lines[i]
                if l.startswith(("diff --git ", "@@")):
                    break
                if l.startswith("+"):
                    after.append(l[1:])
                elif l.startswith("-"):
                    before.append(l[1:])
                elif l.startswith(" "):
                    before.append(l[1:])
                    after.append(l[1:])
                elif l == "":
                    # A context line that was empty loses its leading
                    # space in some tools; treat it as one.
                    before.append("")
                    after.append("")
                elif l.startswith("\\"):
                    pass                     # "\ No newline at end of file"
                else:
                    break
                i += 1

            if is_new:
                new_files.append({"file": path, "lines": after})
            else:
                edits.append({
                    "file": path,
                    "in": context,
                    "find": before,
                    "replace": after,
                })
            continue

        i += 1

    return new_files, edits


def patch_header(text):
    """The commented preamble, which names the upstream commit."""
    upstream = commit = None
    for line in text.splitlines():
        if not line.startswith("#"):
            break
        m = re.search(r"Upstream:\s*(\S+)", line)
        if m:
            upstream = m.group(1)
        m = re.search(r"Applies to:\s*(\S+)", line)
        if m:
            commit = m.group(1)
    return upstream, commit


# --------------------------------------------------------------- applying

def find_block(haystack, needle, start=0):
    """Every place `needle` appears as consecutive lines."""
    if not needle:
        return []
    hits = []
    n = len(needle)
    for i in range(start, len(haystack) - n + 1):
        if haystack[i:i + n] == needle:
            hits.append(i)
    return hits


def squash(lines):
    """Whitespace flattened, for a second look when the exact text fails.

    Reindentation is the most common harmless change to code somebody
    else maintains, and refusing an edit over it would send people to
    the fork for no reason. A match found this way is applied and said
    out loud, because it is a weaker claim than an exact one.
    """
    return [re.sub(r"\s+", " ", l).strip() for l in lines]


def symbol_present(lines, symbol):
    """Whether the function a hunk lived in is still recognisable.

    Only the name is looked for, not the whole signature: a parameter
    added to a function is not the function going away, and saying so
    would be the wrong diagnosis.
    """
    if not symbol:
        return None

    # The name is what stands immediately before the argument list, not
    # the last identifier in the signature -- that would be the last
    # parameter's, `len` in `int len`, which is present in every file
    # ever written and would report every rename as "still there".
    head = symbol.split("(", 1)[0]
    m = re.findall(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*", head)
    if not m:
        return None
    name = m[-1].split("::")[-1]
    if name in ("void", "int", "bool", "static", "const", "if", "class",
                "struct", "auto", "inline", "unsigned", "else", "for"):
        return None
    # On a word boundary: `audioCallback` is a substring of
    # `audioCallbackV2`, and reporting a rename as "still there, body
    # changed" sends somebody looking in the wrong place.
    return re.search(r"\b" + re.escape(name) + r"\b", "\n".join(lines)) is not None


def apply_edit(lines, edit):
    """One anchored edit. Returns (new_lines, status, detail)."""
    find, replace = edit["find"], edit["replace"]

    # Already there? The post-image being present is what "applied"
    # means; checking a marker instead would need one per edit.
    if find != replace and find_block(lines, replace):
        return lines, "already", ""

    hits = find_block(lines, find)
    if len(hits) == 1:
        at = hits[0]
        return lines[:at] + replace + lines[at + len(find):], "applied", ""

    if len(hits) > 1:
        return lines, "ambiguous", f"{len(hits)} places match this anchor"

    # Nothing exact. Try again ignoring how it is indented.
    loose_hits = find_block(squash(lines), squash(find))
    if len(loose_hits) == 1:
        at = loose_hits[0]
        return (lines[:at] + replace + lines[at + len(find):],
                "applied-loose",
                "matched only after ignoring whitespace")

    # Say which of the two failures this is.
    present = symbol_present(lines, edit.get("in"))
    if present is True:
        detail = (f"'{edit['in']}' is still there, but the lines this "
                  f"hooks into have changed")
    elif present is False:
        detail = f"'{edit['in']}' is gone from this file"
    else:
        detail = "the surrounding lines have changed"
    return lines, "failed", detail


def apply_recipe(recipe, root, dry_run=False):
    results = []

    for nf in recipe["new_files"]:
        dest = os.path.join(root, nf["file"])
        if os.path.exists(dest):
            existing = open(dest, encoding="utf-8", errors="replace").read()
            if existing.splitlines() == nf["lines"]:
                results.append((nf["file"], "already", "identical"))
                continue
            results.append((nf["file"], "conflict",
                            "a different file of this name is already there"))
            continue
        if not dry_run:
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "w", encoding="utf-8") as f:
                f.write("\n".join(nf["lines"]) + "\n")
        results.append((nf["file"], "written", ""))

    # Grouped by file so one file is read and written once, whatever it
    # needs, and so two edits to the same file see each other's work.
    by_file = {}
    for e in recipe["edits"]:
        by_file.setdefault(e["file"], []).append(e)

    for path, edits in by_file.items():
        target = os.path.join(root, path)
        if not os.path.exists(target):
            for _ in edits:
                results.append((path, "missing", "no such file upstream"))
            continue

        with open(target, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()

        changed = False
        for e in edits:
            lines, status, detail = apply_edit(lines, e)
            results.append((path, status, detail))
            if status in ("applied", "applied-loose"):
                changed = True

        if changed and not dry_run:
            with open(target, "w", encoding="utf-8") as f:
                f.write("\n".join(lines) + "\n")

    return results


# ------------------------------------------------------------------ main

OK = ("applied", "applied-loose", "already", "written")


def report(name, results, quiet=False):
    counts = {}
    for _, status, _ in results:
        counts[status] = counts.get(status, 0) + 1

    for path, status, detail in results:
        if status in ("applied", "already", "written") and quiet:
            continue
        mark = {"applied": "  ok      ", "already": "  present ",
                "written": "  new     ", "applied-loose": "  ok*     ",
                "failed": "  FAILED  ", "ambiguous": "  AMBIGUOUS",
                "missing": "  MISSING ", "conflict": "  CONFLICT"}.get(status, status)
        print(f"{mark} {path}" + (f"  -- {detail}" if detail else ""))

    bad = sum(v for k, v in counts.items() if k not in OK)
    summary = ", ".join(f"{v} {k}" for k, v in sorted(counts.items()))
    print(f"{name}: {summary}")
    return bad == 0


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("emulator", nargs="?", help="melonDS, azahar or Cemu")
    ap.add_argument("target", nargs="?", help="the checkout to change")
    ap.add_argument("--generate", action="store_true",
                    help="rewrite the recipes from the patches")
    ap.add_argument("--check", action="store_true",
                    help="report what would happen, change nothing")
    ap.add_argument("--quiet", action="store_true",
                    help="only what needs attention")
    args = ap.parse_args()

    patches = os.path.join(here, "patches")
    recipes = os.path.join(patches, "recipes")

    if args.generate:
        os.makedirs(recipes, exist_ok=True)
        for name in ("melonDS", "azahar", "Cemu"):
            src = os.path.join(patches, f"{name}.patch")
            if not os.path.exists(src):
                print(f"{name}: no patch, skipped")
                continue
            text = open(src, encoding="utf-8", errors="replace").read()
            upstream, commit = patch_header(text)
            new_files, edits = parse_patch(text)
            recipe = {
                "emulator": name,
                "upstream": upstream,
                "generated_against": commit,
                "new_files": new_files,
                "edits": edits,
            }
            dest = os.path.join(recipes, f"{name}.json")
            with open(dest, "w", encoding="utf-8") as f:
                json.dump(recipe, f, indent=1)
                f.write("\n")
            print(f"{name}: {len(new_files)} new file(s), {len(edits)} edit(s)"
                  f" -> patches/recipes/{name}.json")
        return 0

    if not args.emulator or not args.target:
        ap.error("give an emulator and the checkout to change")

    path = os.path.join(recipes, f"{args.emulator}.json")
    if not os.path.exists(path):
        print(f"no recipe for {args.emulator}; run --generate first")
        return 2

    recipe = json.load(open(path, encoding="utf-8"))
    results = apply_recipe(recipe, args.target, dry_run=args.check)
    return 0 if report(args.emulator, results, args.quiet) else 1


if __name__ == "__main__":
    sys.exit(main())
