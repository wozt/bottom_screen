# Roadmap

What is left after the proof of concept. What already works is in
[WORKINPROGRESS.md](WORKINPROGRESS.md).

The order is indicative: the dependencies between items matter more than
the numbering. Sound and multi-client touch the protocol, so they come
before multiplying the clients that would have to speak them.

---

## What only a person can do

Everything here is blocked on hardware or on a pair of hands, not on
code. It is listed together because it was scattered across four
sections and easy to lose. Each line says where the detail is.

| | Where |
|---|---|
| Run the homebrew on a real Switch — the picture, then touch, Joy-Cons, sound | [C1](#c1-switch-homebrew-nro--written-unverified) |
| Check the two Android faults on a real phone, which has a hardware decoder | [C6](#c6-android-two-faults-left-open) |
| Play with a physical pad plugged into the host, on each of the three emulators | [B5](#b5-host-controls) |
| Try the other 3DS against the Artic Setup Tool crash | [A3bis](#a3bis-3ds-system-setup-crashes--parked) |
| Hear the sound — there are no speakers behind a virtual display | [C2](#c2-web-app-in-js) |

The first two are the ones that unblock other work; the rest confirm
things that are written and read correct but have never been exercised.

---

## A. Emulators

### A1. Azahar (3DS)
- [x] Backend: reads `screen_infos[2]`, with rotation
- [x] Touch through `TouchPressed` / `TouchMoved` / `TouchReleased`
- [x] Full 3DS profile: ZL/ZR, circle pad, C-stick

### A1bis. Azahar: three faults found under test — 2026-09-08

Found on a commercial 3DS title, internal resolution 6x (1920x1440
streamed from a 320x240 screen), on **all three** renderers — OpenGL,
Vulkan and software — so none of it is renderer-specific. The software
one is too slow to play on in any case and is not a route out.

A trap that cost a test: Azahar rewrites its configuration on exit and
sets `graphics_api\default=true`. In Citra's config system that flag
makes the stored value be **ignored** in favour of the default, so
writing `graphics_api=2` alone silently runs OpenGL again. Write the
flag too, and check what the process actually loaded.

**No sound at all — fixed.** The tap was in `DspInterface::OutputCallback`,
which is not a producer: it is the callback a *sink* invokes to ask for
samples. With no output device configured nothing ever called it, so the
stream was silent on every renderer — which is why changing renderer
made no difference. It moved to `OutputFrame` and `OutputSample`, where
the DSP produces, and above their `if (!sink) return`.

The same move fixed a second fault nobody had been able to hear: the
bridge announces 32728Hz, the DSP's own rate, while OutputCallback
delivers at the sink's, usually 48000. Had it ever run, the sound would
have come out a fifth too low and slow.

**Touch did nothing in the game — fixed, and Cemuhook was not needed.**
The taps arrived and were converted correctly, as the server's own log
said; `EmuWindow::TouchPressed` then threw them away by returning false.
This Azahar runs `layout_option=4`, SeparateWindows, where the two
screens live in two windows and `IsWithinTouchscreen` refuses outright
any touch aimed at the one showing the top screen. All three renderers
called ApplyInput with that window, which is why the renderer made no
difference here either.

The bridge now picks the window that owns the touchscreen, by the
emulator's own rule: the secondary one, unless swap_screen moves it to
the primary. It maps the point through that window's layout too.

**The bottom screen is cut off in the client** — the picture is there
but not whole.

- [ ] Find whether it is cropped on the way out or drawn short

### A2. Cemu (Wii U)
- [x] Backend on `LatteRenderTarget_copyToBackbuffer(_, true)`
- [x] Touch, buttons and sticks wired and verified
- [x] Wii U button profile, sticks included

### A2bis. Cemu: three faults found under test — 2026-09-08

OpenGL renderer, a commercial Wii U title, GamePad at 854x480.

**Touch works.** That is the one thing here that does.

**The streamed sound was badly distorted** — "Canal+ scrambled on match
night" — while the host stayed silent. **Fixed the same day.**

The tap was on `tempTVChannelData`, the group of four DMA blocks, and
that buffer is laid out with a stride taken from the output device:

```cpp
const uint32 channels = g_tvAudio ? g_tvAudio->GetChannels() : AX_TV_CHANNEL_COUNT;
```

With no output device configured — which is this machine, and why the
host is silent — `g_tvAudio` is null and the stride falls back to six
channels while a stereo title writes two. The group is then real audio
separated by samples nobody ever wrote, and we were reading it whole.
Hence a sound that was neither silence nor noise but both, alternating.

The tap moved to the DMA block itself, immediately after the endianness
conversion, where it is contiguous and carries its own channel count.
It no longer depends on the emulator having an output at all, which was
always the intent — muting the PC is not supposed to silence the phone.
It also reaches the stream every 3ms rather than every 12ms.

The host being silent is a Cemu setting (`<TVDevice>` empty,
`<PadVolume>` 0), not a fault here.

**Cemu does not see an Xbox pad plugged into the host.** Not in its
controller configuration, and not in the game either — the presses that
looked like the pad working were coming from the web client. The pad
works elsewhere on the machine, so it is Cemu's input backend or its
configuration, and not something this project touches.

- [ ] Configure the pad in Cemu itself, or find why it cannot be

The distorted sound was on **both** renderers and is fixed above. Touch
works on both.

### A3bis. 3DS: system setup crashes — parked

Session of 2026-09-05. The Artic Setup Tool is the only route Azahar
supports for installing the system titles, and it crashes every time.
Everything configurable has been eliminated:

| Checked | State |
|---|---|
| Luma3DS | v13.4, past the v13.3.1 required |
| Artic Setup Tool | v1.0.3, the latest released (April 2025) |
| Confirmation with A on the console | done |
| Partial NAND state | cleaned before the attempt |
| Route in | the dialog **and** the `articinio://` URL |
| Azahar version | `2126.1-rc3` **and** stable `2126.0` |
| Network | never a byte above the noise floor |

So it is neither the network, nor the bandwidth, nor a mistake at the
console.

**What is known about the crash.** A reproducible SIGSEGV. On the
`master` build the stack was:

```
Service::HTTP::InstallInterfaces
 └─ HTTP_C::DecryptClCertA
     └─ NCCHContainer::AutoOpenNCCHNCSD
         └─ UniqueData::GetUniqueCryptoFileKeyIV   (unique_data.cpp:287)
             └─ Certificate::GetPublicKeyECC       ← SIGSEGV
```

`ct_cert` is not read from a file: it is **derived from the OTP** by
`BuildECC()` (unique_data.cpp:180), checked against the root key on line
184, and invalidated if that check fails. The guard on line 292 tests
`IsValid()`. So the certificate passes the check and then breaks when
its public key is read — an inconsistency between what the check accepts
and what the read assumes.

On `2126.0` the crash comes earlier still, before the unique data is
even written. The stack above belongs to the other binary; a fresh `gdb`
pass on the stable one is needed before fixing anything.

**A trap worth knowing.** Every attempt rewrites the unique data
(`otp.bin`, `movable.sed`, `SecureInfo_A`, `LocalFriendCodeSeed_B`)
before crashing. A manual cleanup is therefore undone on the next go,
and starting again without cleaning brings the crash back. Azahar's own
dialog calls `UninstallSystemFiles()` for this reason.

**Leads, cheapest first:**

- [ ] **Try the other 3DS.** If the crash comes from deriving the
      certificate from this particular OTP, another console settles it
      immediately. The most discriminating test and the cheapest.
- [ ] See whether the tool can be used without going through the system
      update — that path, with ClCertA and the NIM module, is the one
      that breaks.
- [ ] Open a ticket with Azahar: there is a clean reproduction and a
      call stack.
- [ ] As a last resort, patch the fork. Not before having the exact
      stack from the binary in question: a guard added blindly would
      move the failure rather than produce the HOME menu.

None of this blocks the project: the Azahar backend can be written
against `FrameDumperOpenGL` and validated later.

**Version note.** The Azahar checkout stayed on tag `2126.0` (detached
HEAD). A dedicated branch is needed before writing the backend, like
`bottom-screen` on melonDS.

### A3. BIOS and system files — blocked

**I will not download them.** The 3DS and Wii U BIOS and firmware are
copyrighted Nintendo files; going and finding them online would be
piracy, and I will not do it even for a private project.

The legitimate route is dumping them from your own consoles. What is
needed, and where to put it:

| Console | Files | How |
|---|---|---|
| DS | `bios7.bin`, `bios9.bin`, `firmware.bin` | **already in place**, in `~/Téléchargements/bios/` |
| 3DS | `boot9.bin`, `boot11.bin`, `seeddb.bin`, `aes_keys.txt` | GodMode9 on a 3DS of yours |
| Wii U | `otp.bin`, `seeprom.bin`, title keys | dump the console |

Put them wherever you like and give me the path, or drop them in
`~/Téléchargements/bios/<console>/`. I will point the emulators at them.
Without them Azahar and Cemu boot nothing, and the backends can only be
tested against a test pattern.

---

## B. Protocol and server

These change the wire, so they come before new clients.

### B1. Sound — done
- [x] Capture the sound of all three emulators
- [x] Encode to Opus and add it to the protocol
- [x] Playback on Android (the Switch homebrew is still to do)
- [x] Volume bar and mute button in the Android menu

Verified end to end: `first audio decoded, 3840 of 3840 bytes written`,
one 20 ms Opus block at 48 kHz written without loss.

Two choices worth knowing. Sound is captured **before** melonDS's own
volume and mute: silencing the emulator on the PC must not silence the
player.

And resampling goes through `swresample`. Opus accepts only 8, 12, 16,
24 or 48 kHz, and the 3DS DSP runs at 32728 Hz — neither one of those
nor even a round rate. Sending raw 32 kHz is therefore impossible, the
conversion is compulsory, and it may as well be correct. melonDS and
Cemu already produce 48 kHz and never cross it; they link the library
all the same, because `bs_audio.c` is shared and linking happens at
build time, not run time.

Sound has its own clock and its own latency; mixing it into the video
path would make one depend on the other. A separate message type, with
its own timestamp, lets each client decide how to synchronise.

### B2. Multi-client
- [x] Several clients at once, the same idea as capture2cloud
- [x] One shared encoder, not one per client
- [x] Input merged across clients (same logic as a pad)
- [x] An explicit refusal when the server is full

Four clients by default (`BsServerConfig::max_clients`). The video loop
went from one per connection to a single one for everybody: the picture
is encoded once and the same packets go to each, so a second viewer
costs bandwidth rather than a core.

Each client does keep its own sending thread and queue, because the one
thing a shared encoder must not do is let the slowest client set the
pace. A phone on bad wifi fills its own queue and, past half a megabyte,
is resynchronised on a keyframe.

That resynchronisation could not be "latest wins" the way raw frames
are: a P frame is a correction to the one before it, so skipping one
leaves the decoder producing noise until the next keyframe anyway. So it
skips deliberately, all the way there.

Buttons are merged with OR across clients, and a client that leaves
while holding one releases it — without which a disconnection in the
middle of a jump leaves A held for good, which looks like a hung
emulator. Touch and sticks stay last-wins: there is only one finger and
one stick to represent.

Verified by `tests/run_multiclient.sh` (three clients at 60 fps each,
then six for four places) and `tests/input_merge.c` (six merge cases,
including the two that fail silently without it).

### B3. Resolution
- [x] Follow the emulators' internal resolution (x2, x4, xN)
- [x] Reconfigure the encoder live when the size changes
- [x] Announce the new size to clients (`STREAM_INFO`)
- [x] GPU readback for melonDS (the last of the three)
- [x] Choice of receiving resolution on the client side

melonDS scales only in its OpenGL renderer, which keeps the screens in a
GPU texture rather than RAM. `GetFramebuffers` then returns `false` and
hands back the handle of an array texture: the top screen on layer 0,
the bottom on layer 1, at 256×N by 192×N. That layer is now read back as
BGRA, the same format the software path produces, so nothing downstream
knows which renderer drew the frame.

The size comes from the texture itself rather than the scale setting —
the opposite shortcut, on the Azahar side, read six times past the end
of a buffer.

Touch is converted from the announced size for the same reason. It
divided by the native 256×192, which is right only at x1; at x4 the
whole screen folded into its top-left quarter. Azahar had this exact
bug.

Verified with a commercial DS title at x4: 1024×768 announced, the bottom
screen the right way up in the right colours, and a tap at the centre of
the announced space starting the game.

**Asking for less.** `BS_MSG_SET_SIZE`, a message type of its own rather
than a wider `BsQuality`, because adding fields to a struct both ends
agree on would break every client that had not been rebuilt. Zero means
"follow the source", which is how a client stops asking.

What a client may ask for is not arbitrary: **whole multiples of the
console's own screen, never below it**, with the Wii U alone allowed to
halve because 854×480 has the room. The first version offered plain
fractions, and a quarter of a 3DS screen is 80×60 — which a hardware
decoder configures without complaint and then produces no frames from at
all, leaving a black screen with nothing in any log to explain it. A
size the console itself could have produced is always decodable.

Shared with everyone watching, like the bitrate and for the same reason:
one encoder, so a size each would mean an encoder each. The server
announces what it settled on through `STREAM_INFO`, since a request can
be rounded to even numbers — YUV420 chroma is half resolution and an odd
dimension has no whole answer — or clamped.

The scaler was already there for the pixel format, BGRA in and YUV out,
so a different output size costs only the resampling. It switches from
point sampling to bilinear when the size actually changes: dropping
pixels out of a picture being made smaller loses thin lines and text,
which on a menu screen is most of what is there.

**The half that goes wrong quietly is touch.** Clients aim in the space
that was announced to them; the backends work in the source's own space.
The conversion happens in the server, the one place that knows both
numbers — leaving it to the backends would put every tap wrong by
exactly the scale, in three different files, and look like a calibration
problem.

A bug that hid behind all this for three attempts: the announcement was
tied to whichever branch of the pump rebuilt the encoder. A quality
change and a size change arriving in the same frame are one rebuild, the
quality branch applied both, and the size branch then saw no difference
and announced nothing — leaving clients drawing and aiming at a size
that no longer existed. It is now announced by comparing against what
was last said, which no branch can get wrong. I had dismissed the
missing log line twice as a testing artefact; it was the symptom.

`tests/run_receive_size.sh` measures rather than assumes it: it taps a
quarter across and a quarter down, because the centre is the one point
that looks right whether or not the conversion happened, and reads the
crosshair back out of the decoded picture. With the conversion the tap
lands at 0.248; without it, at 0.123.

### B4. Port
- [x] If the port is taken at startup, increment and retry
- [x] Port setting in all three emulators
- [x] Switch on by default, in all three emulators

### B5. Host controls
- [x] Written that way in all three bridges
- [x] Actually tried with a pad in someone's hands — melonDS, 2026-09-08:
      the host's pad and the phone's on-screen buttons driving the same
      game at once, both acting

Merging rather than replacing is there by construction everywhere:
melonDS ORs into the local `inputMask`, Cemu into
`is_mapping_down(i) || IsButtonHeld(i)` and into its axes, Azahar into
`state.X.Assign(... || ...)` and after `circle_pad->GetStatus()`. Local
touch keeps priority in each.

But none of that has been exercised with a physical pad plugged into the
host while a client plays, which is the only thing that would prove it.
The box above was ticked on the strength of reading the code, which is
not the same claim.

### B7. A client's buttons must not need a pad on the host

**Required, stated on 2026-09-08.** In all three emulators, a button
pressed on a client has to reach the game whether or not the host has a
controller configured. It is the whole point: the phone *is* the
controller.

**Met, and now demonstrated.** Reported broken on Cemu/OpenGL, then
found working on Cemu/Vulkan minutes later — and the tester afterwards
established what had actually happened: Cemu never responded to the
host's pad at all, and what looked like the pad working was the web
client's buttons driving the game. Which is the proof this asked for.
With no controller configured in Cemu, a client's buttons reach the
game.

Still worth a test that runs with nothing plugged in, because the only
evidence is one session someone happened to interpret correctly.

- [ ] A test that runs with no controller present, so this cannot rot
- [ ] Confirm the same on melonDS and Azahar, which have only ever been
      tried with a pad attached

### B6. Every render backend

- [x] melonDS: software, OpenGL, OpenGL compute
- [x] Azahar: OpenGL, Vulkan, software
- [x] Cemu: OpenGL, Vulkan
- [x] Survive a backend changed while a client is watching

The bridge no longer cares which renderer the emulator uses. Each one
gets the bottom screen into ordinary 32-bit pixels its own way, and
everything after that is shared.

**Cemu / Vulkan** was the awkward gap, since Vulkan is the default on
Linux — the backend most people would actually get was the one that
streamed nothing. The readback is taken from `HandleScreenshotRequest`,
which already does this on that backend and is proven here. It keeps the
alpha channel, leaves the bytes in whatever colour space the buffer
uses, and runs every frame rather than once. Leaving sRGB alone is
deliberate: OpenGL hands back the stored bytes untouched, and a picture
that changed colour because somebody switched graphics API would be a
strange thing to explain.

**Azahar** was hooked in `renderer_opengl` alone, which also said the
bridge belonged to that backend when it belongs to none of them; it
moved to `video_core`. The software renderer already decodes the
framebuffer into RGBA in RAM and transposes as it goes, so it passes its
pixels straight through with no rotation. Vulkan blits into an RGBA8
image of its own first — reading the source directly gave a picture
repeated twice down the frame in the wrong colours, because the image
standing in for the screen belongs to the rasterizer and carries
whatever format the title's framebuffer uses, RGB565 and RGBA4 among
them. It also needs the image and the region, which `ScreenInfo` did not
carry: a view cannot be copied out of, and under accelerated display
that view belongs to a rasterizer surface rather than the texture beside
it.

**melonDS** needed nothing for its compute renderer — that is the 3D
renderer inside the same GL compositor, so it arrives by the texture
path. But melonDS is the one emulator that changes renderer without
restarting, and the software path submitted 256×192 without ever telling
the mailbox: switching back from OpenGL at 2x handed a 256×192 buffer to
a mailbox still sized 512×384, which copies 384 rows of 2048 bytes out
of a 196 KB framebuffer. Both paths now go through one function that
resizes first.

Verified per backend with a real game rather than by reading: a commercial Wii U title
HD on Cemu/Vulkan, a commercial 3DS title on all three Azahar renderers,
a commercial DS title on melonDS software, OpenGL and compute — the same
picture, the same way up, the same colours in each. `tests/resize_flip.c`
covers the size flipping under the sanitizers, with buffers allocated
exactly so an overrun cannot hide in slack.

---

## C. Clients

### C1. Switch homebrew (NRO) — written, unverified

Every line below is written and builds. None of it past the menu has
been seen working, so the boxes say so: **w** for written, **v** for
verified on hardware.

| | Written | Verified |
|---|---|---|
| Menu, rows, saved address | x | x (Citron) |
| Connecting, receiving a stream | x | x (Citron) |
| Hardware decoding (`h264_nvtegra`, software fallback) | x | |
| Drawing the picture | x | |
| Touch as the stylus | x | |
| Joy-Con for the ordinary buttons | x | |
| Sound (Opus, SDL audio) | x | |
| Address entry with the system keyboard | x | x (Citron) |

**The playing screen draws nothing under Citron.** A marker painted as
the very first statement of that function never appears, so the loop is
not reached and it is not the texture or the decoder failing quietly
further down. Ruled out: the hardware decoder (forcing software changes
nothing), opening the audio device, and the picture's format. What is
left is between the connection returning and the loop body.

Citron cannot narrow it further: it refuses the NRO about half the time
and silently discards writes to the SD card, which is the only way to
leave a trace on a console you do not have. **This needs real
hardware**, and everything under it is waiting on the same thing.

- [ ] Run it on a real Switch and find out where the loop stops
- [ ] Then confirm, in order: picture, touch, Joy-Cons, sound

Starting point: `switch_homebrew/` and `switch_stream.c` from
capture2cloud.

### C2. Web app in JS
- [x] A page with the screen, touch and on-screen buttons
- [x] Served by the emulator itself, on the port it already listens on
- [x] Sound
- [x] Saved servers, like the Android client has

**This did not end up being VP8 over WebRTC**, which is what the plan
above said, and the change is worth stating plainly.

The reasoning on record was that a browser goes through WebRTC, where
VP8 is the guaranteed baseline and H.264 depends on the browser, the
platform and sometimes patents. That is true of WebRTC. It is not true
of **WebCodecs**, which decodes H.264 directly — in hardware where the
machine has it — from exactly the bytes the server already produces.

| | VP8 / WebRTC | WebSocket / WebCodecs |
|---|---|---|
| Encoding | a second time, in software, on the machine already emulating | none, the same bytes |
| Code | signalling, ICE, a GStreamer pipeline — 2083 lines in capture2cloud | a WebSocket layer, about 250 |
| Latency | WebRTC's jitter buffer | direct |

The cost is a browser without WebCodecs, which the page says plainly
rather than showing a blank canvas.

**One port, two protocols.** A native client opens with `BsHello`, whose
first four bytes spell the magic `BSC1`; a browser opens with `GET `.
They cannot be confused, so the same listening socket serves both and
there is no second port to explain, forward or get wrong. A plain page
request is answered on the accept thread rather than in a client slot: a
browser fetches the page and then opens the socket, and the fetch has no
business occupying one of the four places.

SHA-1 and base64 are written out rather than linked from libcrypto.
`bs_server.c` is compiled into three emulators, so every library it
touches has to be added to three build systems — which is what adding
libswresample cost.

**The bug that testing found.** The codec string handed to WebCodecs
carries the profile and level, and it means them. A hardcoded
`avc1.42E01E` pins level 3.0: it decoded a DS screen and decoded
*nothing at all*, in silence, for a GamePad at 854×480 — or for anything
at a raised internal resolution. The three bytes after the SPS NAL
header are exactly profile, constraints and level, so the page reads
them out of the first keyframe instead. Verified: `avc1.42c014` for a DS
and `avc1.42c01f` for a Wii U, matching what x264 reports.

**Sound** is Opus through `AudioDecoder`, with no description, because
the server sends bare packets rather than the Ogg encapsulation. Each
packet is scheduled after the last on a running start time; the server
sends one per video frame and they arrive in order, so that is enough
and needs no worklet. Falling behind resets the schedule rather than
piling up, since a queue that only grows turns a hiccup into a permanent
delay.

The status shows the loudest sample seen, for the same reason the video
shows a frame count: a decoder can produce a perfectly well-formed
stream of silence, and "2015 packets" says nothing about whether any of
them carried a sound. It also says **(tap to allow sound)** while the
browser is holding the context suspended, which it does until the page
has been touched — decoded and audible are not the same thing, and
leaving somebody to wonder which they had would be unkind.

Verified in Chromium: 779 of 780 frames decoded for a DS, 840 of 841 for
a Wii U, with the right button set for each console; and 2015 Opus
packets with a peak of 0.10, the warning appearing before a tap and
gone after it. `tests/web_client.py` covers the upgrade, the framing,
the greeting and the input path without a browser.

**Saved servers** took a detour worth recording. Each emulator serves
its page on its own port, and a different port is a different origin, so
the browser gives each one its own `localStorage`: a list saved on the
DS page was invisible from the Wii U page, which is exactly where it was
meant to help. Building it and then watching it fail was how that
surfaced.

So the list rides along in the URL fragment, which the browser never
sends to the server, and the page it lands on merges it. After one hop
every page knows all of them. Names already chosen locally win, because
somebody renamed it for a reason.

The second gap was smaller and just as fatal: "save this one" can only
ever describe the page you are reading, so a list could never reach two
entries. The panel takes a bare port as well — three emulators side by
side are 5090, 5091, 5092, and that is the case every time.

Verified by walking it: save the DS page, add 5980 by port, follow the
link, and the Wii U page comes up with both entries and the stream
running.

One thing I cannot check from here: whether it is actually audible.
There are no speakers behind a virtual display. Every link in the chain
is confirmed — the packets decode, they are not silence, and the context
is running — but the last inch is yours.

- [x] Heard, on both, 2026-09-08

**A column of pixels down the right-hand edge breaks up** on the web
client, watching Cemu at 854x480. Not on Android, on the same stream at
the same moment — so it is the browser's own decode or draw, not the
picture that was sent.

- [ ] Reproduce and find whether it is the canvas or the decoder

### C3. Menu design
- [x] capture2cloud's menu design in the web client
- [x] The same in the Android client
- [x] Keep only what makes sense here: no dongle, no capture card, this
      project uses neither
- [ ] The same in the Switch client

Eleven-pixel monospace on near-black, `#222` on `#444` with `#ddd` text,
a bar that fades out of the way, and menus as a `summary` that reads
like a button with a panel opening **over** the picture rather than
growing the bar downwards — the bar sits on top of what you are
watching, so it must not push into it. The groups are `stream`, `sound`,
`buttons` and `servers`, plus fullscreen; the counters are green beside
them and a warning gets its own place rather than replacing them.

Dropped from capture2cloud, as the brief asks: capture format and
resolution of a capture card, the login, the cloud. None of it exists
here.

**On Android** it is the same design in that client's own idiom: the
dark blue ground, the cyan heading for whatever is open, readouts in a
muted blue-grey, a green monospace line for the stream, and categories
either in columns or opened one at a time. It replaced an AlertDialog
whose real fault was worse than looking different — in landscape it was
shorter than its own contents, so everything from the volume down was
unreachable with nothing on screen to suggest it was there.

The settings button was a large tile in the top-right corner, which in
portrait is squarely over the screen you are playing on. It is small and
dim now, and placed in the black beside or below the picture depending
on which way the phone is held.

Two things came out of doing it rather than reading about it.

The picture was being sized by script measuring the box it sat in, which
is a loop: the canvas grew, the stage shrank around it, and the next
measurement disagreed with the last. capture2cloud's own rule —
`object-fit: contain` — has the browser do it, and cannot distort
because it only ever fits.

But that moves the picture inside its element: the box fills the stage
and the drawing is letterboxed within it. Touch measured against the
element's box would land short by the width of those bands, which on a
4:3 screen in a wide window is most of the width. Corrected, and then
measured rather than trusted: pressing a quarter across and a quarter
down put the crosshair at 0.252, 0.248 — read out of the canvas itself,
so no screen offset is being guessed at.

### C4. On-screen buttons
- [x] Showable and hideable
- [x] Reflect a connected pad when there is one
- [x] Explicit move mode: a yellow frame around the buttons while
      arranging them, like capture2cloud
- [x] The face buttons (A/B/X/Y) stay grouped when moved — the diamond
      moves, not each button
- [x] The same on the web client — shown or hidden, and split down both
      sides in landscape so the picture keeps the full height
- [ ] The same on the Switch client

Three states rather than a checkbox, because the useful default is
neither on nor off: somebody who plugs a controller in wants the buttons
out of the way without being asked, and somebody who unplugs it wants
them back. Hiding them also gives the side band back to the picture in
landscape — 1686 pixels wide instead of 1456 for a GamePad stream, which
is the point of hiding them at all.

A physical pad maps onto the same button codes the on-screen ones send,
so the server cannot tell which sent a press, and the host's own pad
keeps working because the emulators merge rather than replace.

The mapping is positional, not by name. Android labels its buttons the
way an Xbox pad is labelled, where A is the bottom of the diamond; every
console here labels the right one A. So the bottom button sends B and
the right one sends A — verified with injected gamepad events, which
came out as B, A, Y, X, L, R, START, SELECT in that order.

A pad unplugged mid-press releases what it was holding, for the same
reason the server does it for a client that disconnects.

Two things testing turned up:

- the settings dialog was not scrollable, so in landscape everything
  from the volume down was unreachable with nothing to suggest it was
  there. It scrolls now.
- the test pattern drew held buttons as lamps but never named them. A
  button held long enough to see is easy to check on screen; one sent by
  a script lasts a frame, and the lamp is gone before anybody looks.

### C5. Profiles
- [x] Several profiles in the client menu (Android)
- [x] Switch between them when several emulators are running at once
- [x] The same on the web client — the saved list travels in the URL
      fragment, so following a link carries it along
- [ ] The same on the Switch client

Known servers are listed above the address field, one tap each. Saving
happens from the options **once connected**, because that is when the
console is known — the server announces it — and "Wii U GamePad (5410)"
is worth more than the address it replaces. Long press to forget, with a
confirmation: these buttons are meant to be tapped in a hurry.

Two gaps turned up in testing that made the list useless:

- the back button left the application instead of returning to the list,
  so the profiles were only reachable from a cold start. The options now
  offer "Change server".
- relaunching the app with an address while it was running did nothing
  at all: the extras arrived on an intent `onCreate` had already read.
  That is exactly how the GTK launcher (D) drives it, so it would have
  failed silently. `onNewIntent` now switches.

Verified on the AVD: saving both servers, switching by list and by
intent, forgetting a profile, and the old connection being released
server-side.

---

### C7. Audio latency — to fix after the test pass

Reported on 2026-09-08 with both clients watching the same emulator at
once: the sound is **slightly** behind on Android and **badly** behind
in the browser. The picture is not. The point of this project is that it
is live, so a sound that arrives late is a fault and not a taste.

Also heard on melonDS/software: the sound crackles a little on top of
being late.

Look at how capture2cloud handles this before changing anything here —
it is the same author solving the same problem, and it does not have
this.

**Largely fixed on 2026-09-08.** Three separate things were wrong.

The sound was drained inside the video loop, so it left in clumps one
video frame apart: 39% of packets under a millisecond after the one
before, median gap 33ms at 30fps. It has its own thread now — 0%
clumped, median 20.7ms, the Opus cadence.

The browser's schedule was corrected only when it fell behind, never
when it ran ahead, so a stall followed by a rush became a permanent
lead. Bounded both ways.

The stutter that remained was the USB transport: the phone was reached
through `adb reverse`, which is not how anyone uses it. Over Wi-Fi it is
occasional rather than constant.

- [ ] The crackle that is left, which the tester puts down to the
      emulation itself rather than the stream. Worth confirming before
      believing it.

### C8. A client outliving its emulator

Restarting the emulator while the client stays open, then reconnecting,
leaves the client wrong: the picture is broken and **rotation stops
working**. Starting the emulator first and the client second is clean,
every time.

So something from the previous session survives the reconnection —
first suspicion is the announced size, which the client remembers in
order to place touches. Reconnecting has to reset everything the
greeting carries, not merge it with what was there.

Seen on Android against Azahar; whether the web client does the same is
untested.

- [ ] Reproduce, and find what is kept
- [ ] Check the web client for the same

### B8. The encoder is told a frame rate it is not fed

Measured while chasing the bitrate: a requested rate is exceeded by
about 1.4x, and that is the ratio between the frames actually delivered
(42.5/s at the time) and the rate the encoder was configured with (30).
Rate control divides a bitrate by a frame count, so a frame count that
is wrong scales the result by exactly that much.

The number comes from the source: Cemu measures its own output once,
shortly after starting, and never revisits it. A game whose frame rate
moves — and they do — leaves the encoder budgeting for the wrong one.

Not urgent: the picture is right, only the ceiling is soft. It matters
on a phone over Wi-Fi, where asking for 1 Mbit/s and being sent 1.4 is
the difference between fitting and not.

- [ ] Measure delivered fps in the server and rebuild when it drifts
      past some margin, rather than trusting a number taken once

### C6. Android: two faults left open

Both turned up on 2026-09-08 while taking the README screenshots, and
both were seen **only on the emulator's software decoder**
(`c2.goldfish.h264.decoder`). The browser, on the same streams at the
same sizes, shows neither. Whether a real phone with a hardware decoder
is affected is unknown, and that is the first thing to settle.

**A black picture on connecting, intermittently.** The play interface
builds, the profile is right, the server counts the client and streams,
and nothing is drawn. Two real causes were found and fixed:

- `surfaceDestroyed` released whatever decoder was current, but rotation
  creates the new surface before dropping the old one, so it could tear
  down the decoder just built for the new surface. It now releases only
  the decoder belonging to the surface actually going away.
- `decoder`, `decoderHolder` and `surfaceReady` are written on the UI
  thread and read on the network thread with no `@Volatile`. The reader
  was entitled to keep its cached `null` and drop every frame, which is
  why nothing was logged and why forcing a keyframe changed nothing.

Six cold starts passed afterwards where the same case had failed twice
out of two — but it came back later, so a third cause remains.

- [x] **Yes.** One black start in eight on the real phone, so it is not
      an emulator artefact and the remaining cause is real
- [ ] Find it

**A drifted picture.** Not seen on the real phone, in any of the runs
made there — only on the emulator's software decoder, which is where the
suspicion already was.

The test pattern's one moving vertical line leaves a trail of several,
for seconds at a time. What it is not, each
established rather than assumed: not the server (no queue ever
overflowed, in any run), not missing keyframes (11 IDRs measured off the
wire in five seconds, each carrying its SPS), and not the Wii U or the
size of its picture (a DS at 60fps produced four lines while a Wii U
rerun was clean).

A decoded IDR ought to wipe a trail and this one does not, which is the
part I cannot explain.

One hole was closed on the way, which did **not** fix it: the decoder
gave any frame 2ms to find an input buffer and silently dropped it
otherwise, keyframes included — and dropping a keyframe costs every
frame until the next rather than the difference it carried. Keyframes
now get 30ms, and any drop asks the server for a fresh one.

- [ ] Does it happen on a real phone at all?

---

## D. GTK launcher

- [x] A GTK app that preconfigures and launches the three emulators
- [x] Internal resolution (x2, x4, xN) set before launching
- [x] The rest — loading a ROM, the other options — stays the
      emulator's job; we do not reimplement what it already does

`launcher/bs_launcher.c`, GTK3, `make launcher/bs_launcher`. The
emulators are found relative to the launcher, so it works from a
checkout with nothing installed.

The switch and the port travel through the environment
(`BOTTOM_SCREEN`, `BOTTOM_SCREEN_PORT`), which all three now read — Cemu
did not, so that was added. A launch therefore never rewrites a
preference set by hand, and Cemu would throw such an edit away on exit
in any case.

The resolution cannot travel that way, and each emulator keeps it
somewhere else:

- **Azahar**: `resolution_factor` in `qt-config.ini`, written with its
  `\default=false` marker — without which the value is ignored in
  silence.
- **Cemu**: `<pad_size>` in `settings.xml`, with `<open_pad>true`
  alongside — the setting without which nothing is captured at all. The
  graphics API is deliberately left alone now that both backends stream;
  a launcher that quietly changed somebody's renderer would be taking a
  decision that is no longer its business.
- **melonDS**: `ScaleFactor` in `melonDS.toml`, with the OpenGL renderer
  and GL display selected alongside — that is where melonDS scales and
  nowhere else, so a factor without the renderer would do nothing.

Edited files are backed up beside themselves (`.bs-backup`): they hold
game paths and accounts that took someone an evening to set up.

The launcher reads the emulator's output and shows the port **actually
bound**, not the one asked for — they differ as soon as a port is taken.
That, and this machine's address at the top of the window, are what you
type into a client.

It once had a "Send to phone" button that ran `adb shell am start`. That
was never asked for: it was written to drive the Android emulator during
testing, aimed at 10.0.2.2 — the host as seen from an AVD — and a real
phone connects over wifi, where adb is not involved at all. A testing
affordance that leaked into the interface and did nothing useful there.
Removed.

`--set-resolution <emu> <n>` does the same with no window: useful in a
script, and it is how the config writers are tested.

---

## E-bis. Not distributing an emulator

- [x] The main repository contains no emulator source at all
- [x] No BIOS, firmware, keys or games, here or in the patches
- [x] The changes exist as patches as well as forks
- [x] The launcher takes a path to an emulator you installed yourself
- [x] Revisit all of this before anything is made public

**All four repositories are public**, decided on 2026-09-07 with the
trade-offs on the table. Worth recording that it was a decision, not a
default: the project ran private for its first day, and the earlier note
here said it would stay that way until this was revisited. This is the
revisit.

Publishing the forks is what GPL asks for if their binaries are ever
handed to anybody, and it is also the action the risk assessment
flagged as the most exposed. Both halves are true at once.

The line that matters is not "is this an emulator" but "does this
distribute the means to run protected games". It does not, and never
did: this project reads a framebuffer and puts it on a socket. That is
structural rather than something arranged to look well.

What changed is the two places where the shape was worse than the
substance. The launcher used to assume the emulators sat inside a
checkout beside it; it now takes a path to wherever they are actually
installed, and remembers it in
`~/.config/bottom_screen/emulators.conf`. And the emulator changes now
exist as patches (`patches/`) as well as forks, generated by
`scripts/make_patches.sh` rather than maintained by hand.

They still went stale once: nobody edits them, but nobody regenerated
them when the forks moved either, and they spent a while describing a
shape the emulators no longer had — applying cleanly and producing
something that would not build. "Generated, therefore correct" holds
only if something checks, so `tests/run_patches_fresh.sh` does, and
`make test` runs it.

The forks stay, because a branch rebases onto a moving upstream and a
patch does not. The patches are what survives if one of three
repositories on somebody else's servers goes away -- which is the
practical argument, and it holds whatever the legal one turns out to be.

## E. Publishing

- [x] Published on GitHub, publicly — see E-bis
- [~] The mirror on the personal git server (`192.168.2.101:2222`) is
      frozen at 2026-09-07 and behind. GitHub is the only destination
      until it can be realigned

The history was rewritten that day to purge a tooling config and some
over-talkative ignore comments. GitHub took the force-push; GitLab's
`main` is a protected branch and refused it, so the two have diverged
with no common ancestor and ordinary pushes are rejected as well.

Realigning it takes unprotecting `main` there, one force-push of the
branch and the tags, and putting the protection back. Deliberately left
until somebody is at that machine.
- [x] Tags v0.1.0 and v0.2.0 with release notes
- [x] Both published as GitHub Releases, with the three emulator patches
      attached to v0.2.0 so somebody can apply them without cloning a
      mirror
- [x] README pointing at the emulator forks (`wozt/melonDS`,
      `wozt/azahar`, `wozt/Cemu`), which are public too
- [x] `goal.md` renamed to `prompt.md` and kept out of the repository
