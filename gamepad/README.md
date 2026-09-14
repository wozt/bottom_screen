# An emulator's screen, on a real Wii U GamePad

A client like any other: it connects to an emulator's server, decodes
the stream, and sends touch and buttons back. What is different is where
the picture goes — over the air to the GamePad itself, through
[libdrc](https://github.com/wozt/libdrc) and a Realtek adapter
pretending to be a Wii U.

Which closes a circle worth naming: a Wii U GamePad, driven by an
emulator running a Wii U game, on a PC, with no console anywhere. Or a
3DS on a GamePad, if that is what the emulator happens to be.

**This is a skeleton.** It compiles, it connects, it decodes, it scales,
and the shape is right. What it has never done is talk to a GamePad —
there is none on the machine it was written on. The parts that are
unknown rather than merely unwritten are marked `TODO` in the source,
each saying what is unknown.

---

## What has been tested, and what has not

| | |
|---|---|
| Connects, decodes, scales to the panel | yes — 848x480 and 1280x720 both, at 58 frames a second |
| Follows a change of screen and of size | yes — the decoder is rebuilt, which is what every other client here learned to do the hard way |
| Sends a picture to a GamePad | **yes** — a game, from Cemu, 0 resync sustained |
| Sends sound to a GamePad | pushed, never listened to |
| Buttons, sticks and touch coming back | sticks confirmed, the rest untried |

`--no-pad` runs everything except the GamePad. It exists so that a
fault in the half that can be tested is not blamed on the half that
cannot -- and it earned that on the first run, where the pad could not
decode a game and the same bridge fed the built-in test pattern decoded
perfectly. That one comparison moved the fault out of this code and into
the encoder's settings in a single step.

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

The GamePad's panel is 864x480. Nothing this project serves is that
size — a Wii U GamePad screen out of Cemu is 848x480, a 3DS top screen
is 400x240 — so the scaling happens here rather than by asking the
server for 864. That keeps the server's sizes whole multiples of the
console's own screen, which is a rule worth more than one client's
convenience.

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

- **Everything involving an actual GamePad.** See the table above.
- **A dead zone on the sticks.** Every axis is sent on every poll,
  including whatever a stick at rest drifts by. The other clients here
  send on change only.
- **The sound has been pushed but never heard.** The Opus packets are
  20 ms and libdrc wants 8, so the remainder is carried between packets
  rather than padded with silence; whether that is audibly right is a
  question for ears.
- **The poll rate**, currently 200 Hz because that is comfortably above
  the GamePad's own 60. What `PollInput` actually updates at is unknown.
- **`BS_MSG_PROMPT` is ignored.** A 3DS asking for a name has nowhere to
  go on a GamePad unless a keyboard is drawn into the picture, which
  libdrc does not offer and this would have to draw itself.
- **Mono sources are upmixed by repeating the sample.** No emulator here
  sends mono, so it has never run.

---

## Licence

libdrc and x264 keep their own; see their trees. Everything written here
is this project's.
