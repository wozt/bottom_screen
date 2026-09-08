# The Switch client

Builds with devkitA64 and libnx:

```sh
cd switch && make          # produces bottom_screen_switch.nro
```

Copy the `.nro` to `/switch/` on the console's SD card and launch it from
the homebrew menu.

The address is remembered in `/switch/bottom_screen/server.txt`, one line
of `host port`. It is written by the app when you set it with X and Y,
and read at startup — a saved address is connected to straight away,
because on a console you launch this to play rather than to look at a
menu.

## Status

**Verified on a real Switch on 2026-09-08**, and it worked first time:
picture, sound, latency, buttons and touch. Everything had been written
against an emulator that would not run it, so this was a leap rather
than a step.

It also carries an on-screen pad, off by default -- a Switch has every
button a DS or a 3DS has, so it is there for the ones it has not got (a
Wii U's HOME) and for playing with the tablet in two hands and the
Joy-Cons off. The pad came from capture2cloud's own homebrew rather than
being written again here.

The playing screen never drew anything under Citron and the reason is
still unknown. It no longer matters: Citron refuses the NRO about half
the time and silently discards writes to the SD card, which is the only
way to leave a trace on a console you do not have, so it was never going
to answer. The console did, immediately.

## What is shared

`bs_protocol.h`, `bs_net.c` and `bs_decoder.c` are the same files the
desktop client uses, so the wire format and the framing cannot drift
between the two. Only what is genuinely different lives here: the
console's video block, its pad, its touchscreen and its keyboard.

Two portability fixes came out of building it, both in shared code and
both improvements on their own terms:

- `bs_net.c` included `sys/uio.h`, which the Switch does not ship though
  it has `struct iovec` and `sendmsg`. The include is now conditional.
- `bs_connect` sent every address through `getaddrinfo`. An address
  typed as four numbers is already an address, so it now goes straight
  to `connect`. On a PC that saves a lookup; on the Switch it is the
  difference between working and not, because `getaddrinfo` fails there.
