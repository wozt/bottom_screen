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

**The menu works and looks right**, checked in Citron: rows in
capture2cloud's own palette, a blue accent on whatever is selected,
driven with the pad. It builds, reads its configuration off the SD card,
and connects to a server, which then counts a client and sends frames.

**The playing screen draws nothing under Citron**, and I did not find
out why. What is established:

- rendering works — the menu draws, and Citron reports a steady 60 fps;
- the connection works — the server sees a client and streams to it;
- the drawing loop is never reached once connected. A marker painted as
  the very first thing in that function never appears, so it is not the
  YUV texture or the decoder failing quietly further down.

Ruled out along the way: the hardware decoder (forcing the software one
changes nothing), opening the audio device, and the picture's own
format. What is left is somewhere between the connection returning and
the loop body, which is a short stretch I could not narrow further from
outside.

Citron is a poor instrument for this in any case. It refuses to load the
NRO about half the time with "Could not determine title ID", and it
silently discards writes to the SD card — which cost an afternoon,
because the obvious way to trace a console with no console is to leave
breadcrumbs on its card.

So: the menu is verified, the stream reaches the console, and the
picture needs real hardware. Touch, the Joy-Cons and sound are untested
for the same reason.

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
