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

Verified: it builds, runs, reads its configuration off the SD card,
connects to a server and receives a stream — checked with Citron on the
development machine, where the server counted a connected client and
1500 frames sent.

**Not verified: the picture.** Citron shows black. The client asks
libavcodec for `h264_nvtegra`, which is the console's own video block,
and an emulator has no such block to offer — so this is the expected
place for it to fail and says nothing either way about real hardware. It
needs a real Switch, which is where it is going next.

Also untested for the same reason: touch, the Joy-Cons and sound.

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
