# Bottom Screen

Stream a Nintendo console's bottom screen out of an emulator and onto a
phone, with touch and buttons travelling back the other way.

Three consoles, three emulators, one protocol: melonDS for the DS,
Azahar for the 3DS, Cemu for the Wii U. The client is not told which one
it is talking to in advance — the server announces the console, the size
and the buttons that exist, and the interface builds itself from that.

---

## The emulators

This only works with **the forks**, on the `bottom-screen` branch. The
upstream emulators have no bridge.

| Console | Fork | Branch |
|---|---|---|
| Nintendo DS | [wozt/melonDS](https://github.com/wozt/melonDS) | `bottom-screen` |
| Nintendo 3DS | [wozt/azahar](https://github.com/wozt/azahar) | `bottom-screen` |
| Wii U | [wozt/Cemu](https://github.com/wozt/Cemu) | `bottom-screen` |

Each fork expects to find this repository beside it:

```
bottom_screen_server/
├── bs_server.c, bs_encoder.c, …      the core, in C
└── emulators/
    ├── melonDS/
    ├── azahar/
    └── Cemu/
```

All three compile the same C files. The per-emulator bridge is the only
C++ in the path, and it exists only because their APIs are.

---

## Building

```sh
sudo apt install libavcodec-dev libavutil-dev libswscale-dev \
                 libswresample-dev libsdl2-dev libgtk-3-dev
make
```

That gives the standalone server (with a test pattern, so the pipeline
can be worked on without an emulator), the Linux client and the
launcher.

The emulators build normally, with their own instructions. The bridge
turns itself on.

---

## The launcher

```sh
make launcher/bs_launcher && ./launcher/bs_launcher
```

It sets the three things that have to be right before each launch —
stream, port, internal resolution — and nothing else: loading a game or
mapping a pad is still the emulator's job.

The switch and the port travel through the environment
(`BOTTOM_SCREEN`, `BOTTOM_SCREEN_PORT`), so a launch never rewrites a
preference somebody set by hand. The resolution cannot: it lives in
three different formats, each with a trap of its own — Azahar's
`\default` marker, melonDS's OpenGL renderer, Cemu's pad view and
graphics API. The launcher knows about all of them. Files it edits are
backed up beside themselves.

The port it shows is the one **actually bound**. A server whose port is
taken moves to the next, which is what lets the three emulators run
together.

`--set-resolution <emulator> <n>` does the same with no window.

---

## The clients

**Android** — `android/`, Kotlin, hardware decoding through MediaCodec
straight into a Surface. Movable on-screen buttons, sticks where the
console has them, sound with volume and mute, fullscreen, and a list of
known servers so an address never has to be typed twice.

```sh
cd android && ./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

**Linux** — `bottom_screen_client`, SDL2, handy for checking a pipeline
without a phone.

```sh
./bottom_screen_client --host 192.168.1.20 --port 5090
```

Four clients can watch at once. The picture is encoded once for all of
them, and their buttons are combined rather than fighting.

---

## What works

Video, touch, buttons, sticks and sound, from all three emulators to a
phone.

Internal resolution follows on all three: raising the render scale
changes the size of the stream, and the server renegotiates with the
clients already connected instead of dropping them.

---

## What does not work yet

- **The 3DS system menu.** Azahar crashes when the Artic Setup Tool
  connects, so the system files are not installed. Games and homebrew
  run.
- **Cemu under Vulkan.** Reading the GamePad view back only exists on
  the OpenGL path. The launcher switches the API for that reason.
- **The Switch homebrew and the web client** are not written.

The detail is in [ROADMAP.md](ROADMAP.md).

---

## The protocol

One header, [bs_protocol.h](bs_protocol.h), included by both ends. TCP,
port 5090 by default, H.264 for the picture and Opus for the sound.

One thing that costs dearly when forgotten: touch coordinates are
expressed in **the space the server announced**, not the console's
native size. Dividing by the latter puts every tap wrong by exactly the
resolution scale, and it looks like a calibration problem when it is
arithmetic.
