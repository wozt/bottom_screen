# An emulator's screen, on a real Wii U GamePad

A client like any other: it connects to an emulator's server, decodes
the stream, and sends touch and buttons back. What is different is where
the picture goes — over the air to the GamePad itself, through
[libdrc](https://github.com/wozt/libdrc) and a Realtek adapter
pretending to be a Wii U.

Which closes a circle worth naming: a Wii U GamePad, driven by an
emulator running a Wii U game, on a PC, with no console anywhere. Or a
3DS on a GamePad, if that is what the emulator happens to be.

**It works**, and it was a skeleton for about an hour. A game running in
Cemu reaches a real GamePad with no keyframe requests sustained, which
is the number that decides whether a picture holds or freezes. What is
still untried is listed at the end, and the parts that are unknown
rather than merely unwritten are marked `TODO` in the source, each
saying which it is.

---

## What has been tested, and what has not

| | |
|---|---|
| Connects, decodes, scales to the panel | yes — 848x480 and 1280x720 both, at 58 frames a second |
| Follows a change of screen and of size | yes — the decoder is rebuilt, which is what every other client here learned to do the hard way |
| Sends a picture to a GamePad | **yes** — a game, from Cemu, 0 resync sustained |
| Sends sound to a GamePad | audible on the physical pad after reassociation with transport active |
| Buttons, sticks and touch coming back | confirmed with the interactive pattern |

`--no-pad` runs everything except the GamePad. It exists so that a
fault in the half that can be tested is not blamed on the half that
cannot -- and it earned that on the first run, where the pad could not
decode a game and the same bridge fed the built-in test pattern decoded
perfectly. That one comparison moved the fault out of this code and into
the encoder's settings in a single step.

## Launcher and interactive pattern

Run `scripts/bottom-screen` (or the local `bs` shell alias). It builds the
launcher, pattern server and GamePad client. Choose **Sync GamePad** for
DS, 3DS or Wii U, then **Launch**. The launcher keeps the pattern running
on a separate port until the emulator announces its actual server port.
When the emulator exits, the pad returns to that console's pattern.

The launcher's **GamePad Wii U** panel holds the pad's own settings --
screen, a resolution for each screen, the scaling filter, sharpness and
the bitrate asked of the server. They are starting values passed on the
command line (`--screen`, `--resolution`, `--top-resolution`, `--filter`,
`--sharpness`, `--bitrate`); the menu on the pad owns every later change
and is the only thing that writes them to disk, so a choice made in the
launcher for one session does not become the saved default. They live
there rather than on each emulator's panel because there is one physical
pad and they follow it whichever emulator it is showing.

Only one GamePad client runs at once. Each source change stops the old
client, starts the new A/V transport, and deauthenticates the pad while
continuing to feed that transport. Waiting until *after* reauthorization
to start the streamer left audio silent on this bench. WPA reauthorization
is checked through hostapd's `[AUTHORIZED]` flag, including quick reconnects.

The AP interface is detected when `/var/run/hostapd` contains one socket.
Use `--iface` / `BS_PAD_IFACE` if there are several. `--hostapd-cli` /
`BS_HOSTAPD_CLI` overrides the CLI; otherwise the local
`~/rtw88_TSF/drc-hostap/hostapd/hostapd_cli` or PATH is used.

Pattern controls:

- Sticks change RGB colour and brightness.
- The 15 mapped buttons play distinct notes, starting at 440 Hz.
- D-pad up/down change the octave (−1 through +2); bottom-right lamps show it.
- Touch moves the circle and plays a continuous tone while held: X controls
  pitch, Y adds a harmonic. Releasing fades the sound out.
- Voices are mixed with bounded gain to avoid clipping chords.

The synthetic audio is paced at 48,000 samples/s. Generating an unlimited
chunk on every drain previously filled libdrc's queue with old silence.
`--stats` now also reports decoded audio packet/sample counts and peaks.

Validation: `make tests/testpattern_input tests/launcher_session`, then
`./tests/testpattern_input` and `xvfb-run -a ./tests/launcher_session`.
The second test checks source switching and prevents overlapping clients.

## Touch menu, screens and stick drift

Tap the small 8 × 8 pixel marker in the top-right corner of the physical
GamePad screen. The full-screen local menu has four corner choices; moving
a touch into a corner after starting elsewhere does not open it.

**Top or bottom screen** is a button on the menu's title row, beside
Close, so it is reachable from either page. There is no controller
shortcut for it, here or on any other client -- the browser has a
dropdown, the Switch a settings row, Android a menu toggle -- and adding
one only here would be a fourth way to do the same thing. The button
sends the screen, then the size that screen needs, then this client's
bitrate, because each screen has its own encoder on the server built on
the server's defaults.

Touch is sent only while the bottom screen is showing. A top screen has
no digitiser to pretend to be.

**Both sticks** have a central radial deadzone and a centre of their own.
A selector at the top of the Commandes page says which one the controls
below it act on; there is one set of widgets, not two copies to keep in
step. Adjust the deadzone with −/+ in 1% steps (0–40%). Deflections
inside it send zero; values outside are smoothly rescaled to retain full
travel. The graph shows raw input in orange and corrected input in cyan,
for the selected stick. Optional centre calibration averages 1.2 seconds
after a 0.3-second settling delay; keep the stick released. Moving or
disconnected samples are rejected.

The **right** stick starts at 12% because this pad's right stick drifts.
The **left** starts at **0%**, because applying 12% to a stick nobody
had complained about would have been a silent change to how a game
plays. Reset restores each stick's own default.

Corner, deadzone and centre are saved automatically to
`$XDG_CONFIG_HOME/bottom-screen/gamepad.conf`, or
`~/.config/bottom-screen/gamepad.conf`. They persist across source changes.
The right stick's keys keep the names they were written under, so a file
from before the left stick existed still loads and still means the same
thing. The chosen screen is not saved: `--screen` on the command line is
what a launcher sets, and a view is not a setting.
While the menu is open, controls are released to the emulator, menu touches
are consumed, and held buttons must be released before they can reach the
game again. The menu redraws independently of incoming emulator frames;
opening it does not restart the stream or reauthenticate the pad.

Run `make -C gamepad test` for both sticks' deadzones, calibration, the
screen button, gesture isolation and settings persistence.
`tests/run_gamepad_aspect.sh` -- part of the top-level `make test`, and
skipped where the bridge is not built -- checks all six console-and-screen
combinations against a running server, both the size asked for and the
rectangle drawn. The client additionally requires Cairo headers
(`libcairo2-dev`) to render this menu.

## Asking for more than the panel

The Image page's **Résolution demandée** is a multiplier over the box
that screen gets on the panel -- ×0.5, ×0.75, ×1, ×1.5, ×2 -- and the
row names the pixels it works out to for the console in hand. On a DS
that is 320x240 through 1280x960, with ×1 at 640x480.

Above ×1 this is supersampling: more pixels arrive than the panel has
and the scaler here brings them down, which is sharper than asking the
server's scaler for the same picture. It only helps while the emulator
is rendering above the panel -- at a console's own native resolution
there is nothing up there to fetch -- and the server's 1440-line ceiling
still applies whatever is asked for.

The letterbox is computed from the console's shape, not from the size
that arrives. Requests are rounded to whole macroblocks, so three
quarters of a 4:3 box is 480x368 rather than 480x360, and letterboxing
to what arrives would carry that 2% into the picture.

**It is set per screen**, and changes take effect live. The two screens
are different pictures with different encoders behind them -- on a 3DS
the top carries the game and the touch screen carries a map -- so there
is no reason for one to pay for the other's sharpness. The row names
which screen it is about, and the screen button switches both the
picture and the setting.

The settings file names them `resolution` and `top_resolution`. The old
`detail` key is not read: it numbered a local blur where 0 meant none,
and reading it under the new meaning would have quietly halved the
picture of everybody who had ever opened the menu.

## The preset, and why it is not libdrc's

`resync` is the number that matters: it counts the keyframe requests the
GamePad sends when it cannot decode. Zero means it is decoding cleanly;
anything near the frame rate means the picture is about to freeze.

Measured against a running game, twenty seconds each:

| preset | resync/s | packets an image |
|---|---|---|
| `medium` | **60** | up to 12 |
| `fast` | 0 | 5 |
| `veryfast` | 0 | up to 8 |
| `ultrafast` | 0 | 5 |

libdrc's own note says medium or below, and that was measured on a flat
test pattern. A game is heavier, and medium is not enough for one. This
bridge sets `fast` by default; `DRC_PRESET` set by hand still wins.

```sh
./bs_gamepad --host 192.168.1.20 --port 5090 --no-pad
```

---

## Running it for real

The whole stack has to be up first: the driver loaded with
`disable_ips=1`, the AP running, and the GamePad paired. All of that is
[docs/WIIU_GAMEPAD.md](docs/WIIU_GAMEPAD.md), which is the measured
account of getting there and is worth reading before anything here.

```sh
./bs_gamepad --host 192.168.1.20 --port 5090
./bs_gamepad --host 192.168.1.20 --port 5090 --screen top
```

The GamePad's panel is 864x480 and nothing this project serves is that
shape, let alone that size. So the bridge works out the largest
rectangle of the console's own shape that fits the panel, asks the
server for exactly that, and pads the sides with black:

| | Shape | Server is asked for | Drawn at |
|---|---|---|---|
| DS, either screen | 4:3 | 640x480 | 640x480 at x=112 |
| 3DS touch screen | 4:3 | 640x480 | 640x480 at x=112 |
| 3DS top screen | 5:3 | 800x480 | 800x480 at x=32 |
| Wii U, either screen | 16:9 | 848x480 | 848x480 at x=8 |

The 3DS is the only console whose two screens differ, which is why the
request is recomputed on every change of screen rather than kept from
the handshake -- and why the screen has to be sent *before* the size.
The server files a size under whichever screen the client is on when it
arrives, so the other order sized the screen being left.

Touch is aimed inside that rectangle, not across the panel. Reading a
tap across all 864 pixels when the picture is 640 wide puts it a fifth
off and worse towards the edges, which reads as a calibration fault when
it is arithmetic. A tap in the black bars is not on the picture and is
dropped.

Asking for 864x480 whatever the console is was the first attempt, and it
is worth saying why it was wrong: the *server* then does the stretching,
so a 4:3 DS screen arrives already a fifth too wide and no amount of
letterboxing here can undo it. The circles are ovals by the time they
reach this program. Every size in that table is a whole number of
macroblocks, so the server encodes it directly and the bridge is left
with a colour conversion and two black bars.

---

## What is in vendor/

Two build outputs of two other projects, carried here because getting
them is half the work:

| | From | Why it cannot be the system one |
|---|---|---|
| `vendor/libdrc` | [wozt/libdrc](https://github.com/wozt/libdrc) | the protocol itself; there is no packaged version of it |
| `vendor/x264` | [wozt/drc-x264](https://github.com/wozt/drc-x264) | x264 with DRH slicing and no slice header, which the GamePad requires and upstream x264 has no notion of |

The build links both by rpath, so it runs from this directory without
anybody setting `LD_LIBRARY_PATH`.

A machine with ffmpeg also has a system x264, and both end up loaded in
the same process — libdrc needs the modified one and libavcodec needs
its own. That is safe, and checked rather than assumed: x264 stamps its
build number into every exported symbol, so libdrc asks for
`x264_encoder_open_140` and only the vendored library defines it. The
system's `164` cannot answer.

---

## Building

```sh
sudo apt install libopus-dev libavcodec-dev libavutil-dev libswscale-dev
make
```

Not part of the top-level `make`: it needs libdrc and a C++ compiler,
and a machine with no Realtek adapter has no use for it. The top level
stays buildable everywhere.

---

## What is left

- **The poll rate**, currently 200 Hz because that is comfortably above
  the GamePad's own 60. What `PollInput` actually updates at is unknown.
- **`BS_MSG_PROMPT` is ignored.** A 3DS asking for a name has nowhere to
  go on a GamePad unless a keyboard is drawn into the picture, which
  libdrc does not offer and this would have to draw itself.
- **Mono sources are upmixed by repeating the sample.** No emulator here
  sends mono, so it has never run.
- **The added video delay** is drawn in the menu and saved, but nothing
  reads it yet. Sharpness, the scaling filter, the bitrate and the
  resolution all reach the picture; that one row does not.

---

## Licence

libdrc and x264 keep their own; see their trees. Everything written here
is this project's.
