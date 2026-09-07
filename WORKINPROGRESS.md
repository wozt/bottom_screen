# bottom_screen_server — work log

Stream the bottom screen of Nintendo consoles (DS, 3DS, Wii U) out of
their emulators and onto an Android phone or a Switch 1 homebrew, with
touch and on-screen buttons travelling back.

The original brief is `prompt.md`, kept out of the repository.

---

## State

Video, touch, buttons, sticks and sound work on all three emulators.
Internal resolution follows on all three. Four clients can watch at
once. A GTK launcher sets everything up.

What is left is in [ROADMAP.md](ROADMAP.md); the two large pieces are
the Switch homebrew and the web client.

---

## Settled decisions

**No window capture.** The bottom screen's framebuffer is taken inside
the emulator, before composition. An X11 or Wayland capture would add a
copy, a conversion and latency, for a less reliable result — an occluded
window, a changed layout, and so on.

**H.264, not VP8.** Both target clients — Android and Switch 1 — have a
hardware H.264 decoder. Encoding costs nothing at these resolutions; it
is decoding on the client that matters.

**Mirrors, not forks.** GitHub does not allow a fork of a public
repository to be private, and the three started out private, so they are
copies pushed by hand rather than forks. They are public now, but the
shape stayed: no pull request upstream is possible from a mirror, and
resyncing happens manually through the `upstream` remote.

**Native resolutions, never stretched.** The server sends the native
resolution; the client may enlarge it, but the decoded framebuffer stays
native.

| Console | Bottom screen |
|---|---|
| Nintendo DS | 256 × 192 |
| Nintendo 3DS | 320 × 240 |
| Wii U GamePad | 854 × 480 |

Checked in `emulators/azahar/src/core/3ds.h:16-19`: the 3DS bottom
screen really is 320×240; it is the **top** one that is 400×240. The
400×240 often quoted for touch is a common mistake.

---

## Hook points in the code

### melonDS — the simplest of the three

| What | Where |
|---|---|
| Separate top / bottom framebuffers | `src/GPU.h:75` — `GPU::GetFramebuffers(void** top, void** bottom)` |
| Touch injection | `src/NDS.h:419` — `NDS::TouchScreen(u16 x, u16 y)` |
| Touch release | `src/NDS.h:420` — `NDS::ReleaseScreen()` |
| Buttons | `src/NDS.h:422` — `NDS::SetKeyMask(u32 mask)` |
| The loop where it is all applied | `src/frontend/qt_sdl/EmuThread.cpp:258` |

`GetFramebuffers` returns `true` when the framebuffers are in RAM and
`false` when the renderer is on the GPU — in which case the values are
renderer-specific. Both cases are handled; see "GPU readback" below.

`EmuThread.cpp:258` shows the pattern to follow: the emulation thread
reads `emuInstance->isTouching` / `touchX` / `touchY` every frame.
Injecting network touch means writing those fields from the server — no
need to touch the emulation core.

### Azahar

| What | Where |
|---|---|
| Touch pressed | `src/core/frontend/emu_window.h:200` — `TouchPressed(x, y)` |
| Touch moved | `src/core/frontend/emu_window.h:210` — `TouchMoved(x, y)` |
| Touch released | `src/core/frontend/emu_window.h:203` — `TouchReleased()` |
| Screen geometry | `src/core/frontend/framebuffer_layout.h:28` — `struct FramebufferLayout` with `bottom_screen` and `bottom_screen_enabled` |
| Screen textures | `src/video_core/renderer_opengl/renderer_opengl.h:98` — `std::array<ScreenInfo, 3> screen_infos` |
| GPU readback already written | `src/video_core/renderer_opengl/frame_dumper_opengl.h:33` — `FrameDumperOpenGL`, with PBOs and `PresentLoop` |

`TouchPressed` takes **framebuffer** coordinates rather than screen
coordinates, so `FramebufferLayout` does the conversion — usefully, the
emulator already has it.

### Cemu — the model matches the need exactly

Cemu already renders the GamePad screen separately from the TV, through
a plain `padView` boolean that runs through the whole render pipeline.

| What | Where |
|---|---|
| Copy to the backbuffer | `src/Cafe/HW/Latte/Core/LatteRenderTarget.cpp:865` — `LatteRenderTarget_copyToBackbuffer(textureView, bool isPadView)` |
| TV versus GamePad call | same file, lines 1010 (pad) and 1012 (TV) |
| View geometry | same file, line 828 — `LatteRenderTarget_getScreenImageArea(..., bool padView)` |
| Renderer interface | `src/Cafe/HW/Latte/Renderer/Renderer.h:78` — `DrawBackbufferQuad(..., bool padView, ...)` |
| The game reading VPAD | `src/Cafe/OS/libs/vpad/vpad.cpp:220` — `VPADRead()` |
| Touch validity | `src/Cafe/OS/libs/vpad/vpad.cpp:237` — `tpData.validity` |
| Pad touch state | `src/input/InputManager.h:90` — `MouseInfo m_pad_touch` (position + `left_down`), read through `get_mouse_position(bool pad_window)` |

The touch path was **traced on 2026-09-05**, not assumed:

`src/gui/wxgui/PadViewFrame.cpp:180-185` is what feeds touch. The
GamePad window writes the pointer's physical position, `left_down` and
`left_down_toggle` there, under the structure's mutex. On the other
side, `InputManager::get_mouse_position(bool pad_window)` and
`get_left_down_mouse_info()` (InputManager.cpp:823 and 837) are the
readers.

So it is the same shape as melonDS: write into the fields the frontend
already fills, rather than widening an interface. The game then reads
through `VPADRead`, and `VPADGetTPCalibratedPoint` converts from a raw
0x500 × 0x2d0 space — 1280 × 720, the GamePad's native touch resolution,
not to be confused with its 854 × 480 screen.

---

## Building on Debian 13

### melonDS

Upstream's `BUILD.md` lists only Ubuntu, Fedora and Arch. The Debian 13
equivalent (`libpcap-dev` replaces `libpcap0.8-dev`, which is only a
transitional package):

```bash
sudo apt install extra-cmake-modules libcurl4-gnutls-dev libpcap-dev \
  libsdl2-dev libarchive-dev libenet-dev libzstd-dev libfaad-dev \
  qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-svg-dev
```

```bash
cd emulators/melonDS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

No patches needed, no dependency outside Debian stable. CMake finds
Wayland 1.23.1 and EGL 1.5 on its own. `build/` is already in upstream's
`.gitignore`, so the tree stays clean.

Reference toolchain: cmake 3.31.6, g++ 14.2, ninja, Debian 13.6.

---

## Reusable pieces from capture2cloud

`/home/wozt/dev/capture2cloud` already holds most of the client side and
the transport.

| Piece | File | Reuse |
|---|---|---|
| Native binary protocol | `c2s_protocol.h` | basis for the video + input protocol |
| Switch homebrew client | `switch_homebrew/` | skeleton of the Switch client |
| Switch stream | `switch_stream.c`, `switch_stream.h` | direct socket transport, no WebRTC |
| Native Android app | `android/` | skeleton of the Android client |
| On-screen buttons | `web/`, `page.html` | multitouch overlay logic, d-pad with diagonals |
| Pad remapping | `gamepad_bridge.c` | mapping physical controllers |

---

## Phase 1: measurements

Measured on 2026-09-04, server and client on the same machine, loopback,
DS 256×192 at 60 fps, `libx264` in `ultrafast` + `zerolatency`.

| | |
|---|---|
| Bitrate | 0.75 – 0.84 Mbit/s |
| Frame rate | 60.0 fps, steady |
| Pipeline latency | **0.7 ms** |

What that 0.7 ms contains: BGRA→YUV conversion, encoding, TCP over
loopback, decoding. What it does not: wifi, and presenting on the
client's screen. In other words the codec is not the problem — the
latency budget will be spent elsewhere, which is exactly what needed
knowing before going further.

The bitrate is worth rereading once the source is a real game: the test
pattern is largely static, so x264 compresses it very well.

### Files

| File | Role |
|---|---|
| `bs_protocol.h` | the protocol, included by both ends |
| `bs_source.h` | the seam the emulator backends fill in |
| `bs_encoder.c/.h` | H.264 encoding through libavcodec |
| `bs_decoder.c/.h` | decoding, for the Linux client only |
| `bs_net.c/.h` | TCP transport and message framing |
| `bs_audio.c/.h` | Opus encoding, with swresample |
| `bs_mailbox.c/.h` | the adapter between a pushing emulator and a pulling server |
| `bs_server.c/.h` | the server, as a library |
| `testpattern.c` | synthetic source, also shows the input it receives |
| `bottom_screen_server.c` | the standalone server |
| `bottom_screen_client.c` | the SDL Linux client |
| `launcher/bs_launcher.c` | the GTK launcher |
| `tests/smoke_client.c` | end-to-end check with no screen |
| `tests/input_merge.c` | button merging across several clients |
| `tests/run_receive_size.sh` | a client asking for a smaller picture, and where its taps land |
| `tests/run_patches_fresh.sh` | that `patches/` still matches the forks |

Scratch goes to `/dev/shm` when it exists rather than `/tmp`: the latter
is a tmpfs on most systems but not all, and a few megabytes of test
output has no business being written to somebody's disk.

```bash
make                          # everything
make test                     # every headless check, not one of them
./bottom_screen_server --console ds
./bottom_screen_client --scale 3
```

### Decisions taken while writing the code

**`TCP_NODELAY`, always.** Without it Nagle holds small packets for up
to 40 ms — more than two frames at 60 Hz, and perfectly invisible in a
throughput test.

**No `AV_CODEC_FLAG_GLOBAL_HEADER`.** With that flag the SPS/PPS live
only in the extradata and never appear in the stream: a client arriving
mid-flow, or one that lost the first datagram over UDP, can no longer
configure its decoder. Without it, x264 repeats the headers before every
keyframe and any client can start at the next one.

**No B-frames.** They require holding a picture back to encode the next:
a whole frame of latency for a compression gain nobody needs at this
size.

**The client outputs YUV, not RGB.** SDL uploads YUV straight to the GPU
which converts while drawing. Converting to RGB on the CPU would add a
full-frame pass for something the GPU does for free.

**The input thread is separate from the video loop.** The video loop
spends its time blocked on the next frame's deadline; reading input
there would hold every event until that deadline.

**Aspect kept, fractional zoom.** Revised on 2026-09-04: the first
version allowed only integer multiples, so every source pixel stayed
exactly square. On a phone that cost a third of the screen, for a source
256 pixels wide — there is not much "square" left to protect. The
picture now fills the space with whatever zoom fits.

What stays forbidden is stretching the two axes independently: that
distorts. A fractional zoom does not.

The Linux client's filtering went linear as a consequence. Nearest
neighbour at a non-integer factor makes some source lines occupy two
screen lines and their neighbours one, which shimmers — worse than a
slight blur.

### A bug the test caught

The input thread set the **global** stop flag when a client
disconnected: the server shut down entirely as soon as the first client
left, instead of going back to listening. Fixed with a per-connection
flag. That is the kind of thing a manual test misses — you restart the
server without thinking about it.

Added at the same time: `bs_encoder_request_keyframe()`, called on every
connection. A client that has just arrived has no reference picture and
decodes nothing until the next keyframe, which is up to a second of
black window.

---

## Phase 2: melonDS

Measured on 2026-09-04 on the DS firmware, software renderer, loopback.

| | |
|---|---|
| Frame rate | 60.2 fps |
| Bitrate | 0.57 Mbit/s (menu screen) |
| Luma | min 0, max 255, mean 168 |

The bitrate varies enormously with content: 0.03 Mbit/s on the black
boot screen, 0.57 on the menu. A real game in motion will be far higher
— these numbers are not a forecast.

The luminance statistics exist for a precise reason: a stream can be
perfectly well formed and show nothing at all. Had the emulator handed
over an empty buffer, every frame would decode, at a plausible rate, and
entirely white. That is the difference between "the pipe runs" and "the
pipe carries a picture".

### How touch was proved

The DS firmware stops on a warning screen that waits for a touch.
Sending a tap over the network moved that screen on to the DS menu. That
is hard to argue with: nothing else could have advanced it.

It also exposed a flaw in the test: it sent `TOUCH_DOWN` without ever
sending `TOUCH_UP`. The console therefore saw a stylus set down and
never lifted, and software waiting for a press waited forever. The test
now makes real taps.

### What was changed in melonDS

| File | Change |
|---|---|
| `src/frontend/qt_sdl/BottomScreenBridge.cpp/.h` | new, the only C++ in the path |
| `src/frontend/qt_sdl/EmuThread.cpp` | an include and two hooks |
| `src/frontend/qt_sdl/CMakeLists.txt` | optional integration |

`EmuInstance` is not touched at all. Its input fields are private, but
`EmuThread` is already declared `friend` — hooking there writes into the
same fields as the Qt frontend, at the same moment of the frame, rather
than widening an interface.

The bridge only starts the server on the first frame submitted, so
nothing opens until a game is running.

```bash
BOTTOM_SCREEN=0      # turn it off
BOTTOM_SCREEN_PORT   # listen port, 5090 by default
```

These override the emulator's own settings, which exist too — the
variables are what a scripted launch or the GTK launcher uses.

### GPU readback

melonDS scales only in its OpenGL renderer, and that renderer keeps the
screens in a GPU array texture rather than RAM: `GetFramebuffers`
returns `false` and puts the texture handle in the first pointer, with
the top screen on layer 0 and the bottom on layer 1, at 256×N by 192×N.

For a while the bridge simply warned and streamed nothing there, which
also meant the internal resolution was out of reach. It now reads that
layer back into the same BGRA the software path produces, so nothing
downstream knows which renderer drew the frame.

The size is asked of the texture rather than derived from the scale
setting. The opposite shortcut on the Azahar side — trusting a size that
described the console rather than the texture — read six times past the
end of a buffer.

Touch is scaled from the announced size for the same reason. It divided
by the native 256×192, which is right only at x1; at x4 the whole screen
folded into its top-left quarter, which looks like a calibration problem
rather than the arithmetic it is.

Verified with a commercial DS title at x4: 1024×768 announced, the bottom
screen the right way up in the right colours, and a tap at the centre of
the announced space starting the game.

### The server became a library

The logic lived in a `main()`, which was fine while the only source was
a test pattern. An emulator cannot be reorganised around somebody else's
`main`, so it all moved into `bs_server.c` and runs on its own thread.
The standalone binary is now a thin `main` over the same code — the two
can no longer drift apart.

`bs_mailbox.c` is the joint between the two models. The test pattern
makes a frame when asked; an emulator finishes its frame and moves on,
and cannot be made to wait. The mailbox is latest-wins: if two frames
arrive before the server collects one, the first is lost. That is
deliberate — a queue would trade a dropped frame for growing latency,
and on a screen you are playing on, late is worse than missing.

---

## Phase 6: Cemu

Tested on 2026-09-05 with a commercial Wii U title, a real native Wii U game
in loose files, with no title key at all.

| | |
|---|---|
| Resolution | 854×480, the GamePad's native resolution |
| Frame rate | 30 fps announced, 30.5 measured |
| Bitrate | 2.7 to 6.1 Mbit/s depending on the scene |
| Latency | 3.0 ms, local machine |
| Touch | **verified** — 427,208 sent and received |
| Buttons | **verified** — A received by `VPADController`'s loop |
| Sticks | **verified** — 0.75 deflection received by `VPADController` |

The button trace is worth more than the other two: it sits inside
`IsButtonHeld`, which is called *by* `VPADController`. So it only fires
if Cemu really has an emulated pad — the condition whose absence made
every input vanish in silence. And since touch is read a few lines above
in the same `update()`, that line establishes it too.

Touch was validated with the **Linux client**, not a phone: it already
sends mouse events, and it was exactly the right tool. Full path
confirmed: client → network → server → `m_pad_touch` → `InputManager` →
VPAD.

### A pad must be configured

Without a controller profile Cemu builds **no** `VPADController` at all,
and its `update()` never runs. The bridge can write into `m_pad_touch`
and hold the button and axis state all it likes; nobody reads it — and
nothing says so.

That is what made me announce too early that touch was verified: my
trace sat inside our own bridge, upstream of the consumer. It proved
delivery, not reception.

The minimum is enough, with no physical mapping since the input comes
from the network:

```xml
<!-- ~/.config/Cemu/controllerProfiles/controller0.xml -->
<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
</emulated_controller>
```

### Traps worth knowing

**The GamePad window must be open.** Without it `copyToBackbuffer` is
never called with `isPadView` and nothing is captured — no error, just a
server that does not start. The setting is `open_pad` in `settings.xml`,
and **Cemu rewrites it on exit**: it has to be set before each launch,
or "Open separate pad screen" ticked in the wizard. The CTRL+TAB
shortcut exists but ignores xdotool's synthetic events. The GTK launcher
sets it for you.

**The announced port is not necessarily the one asked for.** If 5090 is
taken the server moves up. I lost several minutes believing something
had failed while Cemu was listening on 5091 — which is why that message
went to stderr, where it is visible even when stdout is redirected.

### A fix that made things worse

The first attempt announced a hardcoded 60 fps for a game running at 30.
The fix measured the rate — during the first seconds, while shaders
compile and caches are cold: it reported **12**.

That was worse than the assumption it replaced. The encoder derives both
its rate control *and* its keyframe interval from that number: at 12 it
spent the whole budget and emitted three times too many keyframes. The
measurement now discards the first 90 frames and counts over the next
90.

---

## A web client, later

Planned, not started: a JS page to play from any browser with nothing
installed.

It will be **VP8, not H.264**, unlike the two native clients. The
reasoning inverts completely with the target:

- Android and Switch decode H.264 **in hardware**, so that is where
  decoding is cheapest. That decided the project's codec.
- A browser goes through WebRTC, where VP8 is the guaranteed baseline.
  H.264 is possible there but depends on the browser, the platform and
  sometimes patents; VP8 works everywhere, immediately.

And capture2cloud already does exactly this: its `gst_webrtc.c` chain
produces VP8 over WebRTC for its browser client, with the signalling and
the input DataChannel already written.

| | |
|---|---|
| Encoder | `bs_encoder` already takes an encoder name, so VP8 is a value rather than a rewrite |
| Transport | WebRTC alongside the binary protocol, not instead of it — the native clients do not want it |
| Input | the same protocol, carried by a DataChannel instead of a socket |
| Interface | the button profiles exist in Kotlin, to be redone in JS |

After the emulators, not before: a third transport on a project whose
first one does not do UDP yet would put the complexity in the wrong
place.

---

## Open questions

**Shared library or duplicated code?** A `libbottomscreen.so` linked
into all three emulators is appealing, but the three have different
build systems and licence constraints. The C sources are compiled into
each instead — which did cost something, twice.

The file list was written out in all three build systems, so adding a
file meant remembering three places. libswresample went into one and was
missed in the others; later `bs_ws.c` broke all three at once and nobody
noticed, because the standalone Makefile still built and nothing rebuilt
an emulator for days. It surfaced by accident.

The list now lives in `bottom_screen.cmake`, beside the sources it
names, and each emulator includes it. Adding a file is one edit. Noticing
that an emulator has stopped building still requires building one.

**Identical frame detection.** Sending nothing when the bottom screen
has not changed (menus, static games) could save a lot. Worth measuring
once everything else is in place.

**Resyncing with upstream.** Our changes live on a `bottom-screen`
branch on each mirror, to be rebased on `upstream/master` periodically.

---

## Licences

| Emulator | Licence | Constraint |
|---|---|---|
| melonDS | GPL-3.0 | sources must be published if modified binaries are distributed |
| Azahar | GPL-2.0 | same |
| Cemu | MPL-2.0 | only the modified files would need publishing |

No constraint while the changes stay local. Private copies are allowed
by all three licences.
