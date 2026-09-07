# The emulator changes, as patches

What this project adds to each emulator, on its own, without a fork
around it.

```sh
git -C melonDS apply patches/melonDS.patch
```

Each patch names the upstream commit it was made against, in its header.

## Why these exist alongside the forks

The forks are where the work happens: a branch rebases onto a moving
upstream and a patch does not, so maintaining these by hand would be
choosing the harder half of the job. They are generated instead, by
`scripts/make_patches.sh`, and cannot drift because nobody edits them.

They exist for two reasons the forks do not cover.

They are three repositories on somebody else's servers, and this project
should not stop existing if one of them does.

And they say plainly what this project changes in an emulator. A fork
does not: a fork is a whole emulator with our work somewhere inside it,
and "what did you actually change" is a question it answers badly.

## What is not in them

The C sources they call into. Those live in bottom_screen_server, which
each emulator's build expects to find beside it — see the main README.
The patch is the seam, not the implementation.

No emulator, no BIOS, no firmware, no keys and no games are in this
repository or in these patches. Bring your own emulator.

## Regenerating

```sh
./scripts/make_patches.sh
```

Verified when generated: each one applies to a clean checkout of the
commit it names, and applying it reproduces the fork's branch exactly.
