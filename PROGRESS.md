# Progress

What works, what does not, and the decisions that would otherwise have
to be rediscovered. One file: this replaces a roadmap, a work log and a
sheet for the top screen, which between them said the same things three
times and disagreed about which was current.

[README.md](README.md) is the front door. This is the ledger.

---

## Where it is

Everything the project set out to do works, on all three emulators and
all three clients: the picture, touch, buttons, sticks and sound, at
whatever internal resolution the emulator is set to, for up to four
viewers at once off one encoder.

Since then it has grown three things the brief did not ask for:

- **the top screen**, streamed beside the bottom one, chosen per client;
- **the machine's own questions** — a name, a message, a Mii — put to
  whoever is watching instead of to the desk the emulator sits on;
- a **launcher**, because three settings in three different places, two
  of them with a trap, is not a thing to do by hand before every run.

| | DS | 3DS | Wii U |
|---|---|---|---|
| Bottom screen | yes | yes | yes |
| Top screen | yes | yes | yes |
| Touch back | yes | yes | yes |
| Buttons, sticks | yes | yes | yes |
| Sound | yes | yes | yes, either output or both |
| Text asked for | — | yes | yes |
| Mii asked for | — | yes | — |

A DS needs no keyboard: its games draw their own on the touch screen,
which already works.

Measured sizes, with each emulator at its own internal resolution
setting — the ceiling is 1440 tall whatever is asked for:

| | Bottom | Top |
|---|---|---|
| melonDS at 4x | 1024x768 | 1024x768 |
| Azahar at 3x | 960x720 | 1200x720 |
| Azahar at 6x | 1920x1440 | 2400x1440 |
| Cemu | 848x480 | 1920x1080 |

A Wii U GamePad is 854 across and every size here is rounded to a whole
number of macroblocks, so it is sent as 848. Every other native size is
already a multiple of sixteen.

The 3DS is the one console whose two screens are different shapes: 4:3
below, 5:3 above. A DS has two of the same, a Wii U two of 16:9.

---

## Open

### The Android picture is sometimes black on connect

About one start in eight on a real phone. Two causes were found and
fixed; at least one remains.

### A requested bitrate is exceeded by about half again

The encoder is told a frame rate measured once at startup and never
revisited. Ask for 4 Mbit/s and roughly 6 arrives.

### Cemu serves nothing on the bottom screen without its GamePad window

The server starts from either view now, so a game running on the
television alone has a port to connect to. What it does not have is a
GamePad picture: Cemu only draws that view when its window is open, so a
client watching the bottom screen sees nothing.

Two honest answers, neither written yet: make Cemu produce that view
without the window, or tell the client the screen is empty instead of
showing it black.

### 3DS system titles are not installed

The only route Azahar supports needs a second console, and the spare has
a swollen battery. Nothing in this project depends on it.

### The top screen is not in every menu yet

Separate quality and size per screen, and the round d-pad, are done on
Android. The browser and the Switch still share one set.

---

## Decisions worth not relitigating

**No window capture.** The framebuffer is read inside the emulator,
before composition. A desktop capture would add a copy, a conversion and
latency for a worse result — an occluded window, a changed layout, a
compositor with opinions.

**H.264 for the picture, Opus for the sound.** Both target clients
decode H.264 in hardware, which is what matters; encoding costs nothing
at these sizes. There is no hardware Opus decoder to court, and Opus is
simply the best thing at these bitrates.

**One encoder per screen, not per client.** A second viewer costs
bandwidth, not a core. It follows that the size and the bitrate are
shared between everyone watching a screen, and the last to ask wins. The
screen itself is the one setting that escapes this, because sharing it
would leave nothing to choose — which is why a second screen costs a
second encoder and the first does not.

**Clients may not ask for keyframes.** One that is struggling asks
constantly, which is exactly when the others can least afford it; three
of them turned the stream into mostly keyframes. The server issues one
itself for the two cases where waiting is half a second of green:
somebody joining a stream that is already running, and somebody moving
between the screens. Measured: 494ms before, 27ms after.

**The card's encoder is the default, and it is tried rather than
assumed.** VAAPI, then NVENC, then QSV, ending at libx264 which always
works -- the first that actually opens wins, because a GPU can have no
encoder and a driver can report one and then refuse every frame. Worth
185% of a core down to 128% at 1080p and 232% down to 134% at 1440p on a
Radeon RX 6600; worth nothing at a DS's own size, where the drawing and
the colour conversion cost more than the encode.

Two things make a default safe here rather than presumptuous. It is
announced at startup, so a stream that looks wrong on a machine nobody
has tested is one line away from being explained. And `--encoder
libx264`, or `BOTTOM_SCREEN_ENCODER=libx264` for the emulators, is the
way back.

Settled once. The default means trying encoders until one opens, and
creating a VAAPI device is not free; every change of size or quality
rebuilds the encoder, so the name that won is kept and used directly
after that.

The bet it rests on is that a hardware encoder repeats SPS/PPS in front
of every keyframe, the way x264 does when asked. That is the driver's
promise rather than ours, so `run_hardware_encoder.sh` measures it: a
client that joins late, having missed the opening keyframe, must still
decode.

**Native resolutions, never stretched.** The server sends what the
emulator renders; a client may enlarge it, but nothing is resampled on
the way. Every size offered is a whole multiple of the screen's own,
because a quarter of a DS screen is 64x48 and a hardware decoder will
accept that and then produce nothing from it.

**Touch coordinates are in the space the server announced**, never the
console's native size. Dividing by the latter puts every tap wrong by
exactly the resolution scale, and it reads as a calibration problem when
it is arithmetic.

**The source list lives beside the sources.** `bottom_screen.cmake`
names the files, and all three emulators include it. It used to be
written out in three build systems, so adding a file meant remembering
three places -- libswresample went into one and was missed in the
others, and later `bs_ws.c` broke all three at once with nobody
noticing, because the standalone build still worked.

**Mirrors, not forks.** GitHub will not make a fork of a public
repository private, and these started private. They are public now but
the shape stayed: no pull request upstream, and resyncing goes through
the `upstream` remote by hand.

---

## Things that cost a day, written down so they cost nobody another

**Sound tapped where it is consumed, not where it is made.** Both Azahar
and Cemu were hooked into a sink's callback, which does not run at all
when the host has no output device — so the stream was silent on a
machine with no speakers, which is exactly the machine this is for. The
tap belongs where the emulator produces the samples.

**A hardware decoder does not follow a resolution change.** The Switch
client wrote down the new size and carried on, which worked for as long
as the only thing that changed it was somebody moving an internal
resolution slider. Changing screens made it a thing people do on
purpose, and the picture simply stopped changing -- the request left,
the server switched, the frames arrived, and nothing decoded. Rebuild
the decoder, and draw at the size actually decoded rather than the size
announced.

**A size announced only on change says nothing to somebody arriving.**
Switch to a screen another client is already watching and nothing is
about to change, so the server said nothing and the new arrival kept the
size from its handshake. One client's picture was decided by another
client's setting, which is what made it look intermittent.

**The `hidden` attribute is enforced by the browser's own stylesheet,
which any author rule beats.** `#prompt { display: flex }` left a
full-screen opaque panel over the page from the moment it loaded,
swallowing every click. `[hidden] { display: none !important; }` goes
first in the file.

**A stream can be correct and still look wrong.** A green stripe down
the right of the picture in a browser: H.264 codes in blocks of sixteen,
854 is 53.4 of them, so the picture is coded at 864 and the padding is
marked to be ignored. ffmpeg honours that and every test here passed;
the browser handed the padding over, and padding nobody wrote is chroma
at zero, which is green. Reading the parameter sets settled it in one
step after two rounds of guessing. The fix is to leave nothing to
honour: resize to a whole number of blocks, so 854 becomes 848.

**The page is compiled into each emulator.** Fixing it and rebuilding
the standalone server fixes nothing for anybody using an emulator, which
is everybody. Rebuild all three, and check the binary rather than
assuming.

**`setsid cmd &` hands back the pid of setsid**, which exits at once --
so every check said the emulator had stopped while it was still holding
a port. Four of them ended up running at the same time. Identify a
process by the port it holds.

**A test that fails for a reason it does not check is worse than no
test.** The second-viewer timing judged its number whether or not there
had been a first viewer, and failed once in a busy suite while passing
every time it ran alone.

---

## Testing

`make test` runs thirteen, in a few minutes, with no console and no game:

| | What it holds down |
|---|---|
| `run_smoke.sh` | a stream decodes, and carries a picture rather than a cleared buffer |
| `run_multiclient.sh` | several clients at once, including a browser's transport |
| `run_receive_size.sh` | a smaller picture asked for, and the tap that lands where it should |
| `run_top_screen.sh` | both screens at once, joining one already running, and the keyframe that saves half a second |
| `run_web_files.sh` | every file the page asks for, parsed |
| `run_prompt.sh` | the machine's question, answered, in both directions |
| `run_hardware_encoder.sh` | the card's encoder, and a client joining late still decoding it |
| `run_switch_client.sh` | the console client's own network half, run on a desktop |
| `input_merge`, `resize_flip` | two clients' buttons merged; the picture changing shape under the encoder |
| `run_patches_fresh.sh`, `run_recipes.sh`, `run_recipe_drift.sh` | the emulator changes still apply, and refuse honestly when they cannot |

Two more are run deliberately, because they want a display, a toolchain
or several minutes:

- `tests/run_recipe_build.sh <emulator>` — takes upstream, applies the
  changes, and **builds** it. Applying and compiling are different
  claims and only the second is worth anything.
- `tests/run_emulator_matrix.sh` — every screen of every emulator
  against the real thing, restarted between each. `bs_server.c` is
  compiled into all three, so an emulator left running is the old server
  wearing the new binary's name.

The console client is tested without a console: `switch/source/stream.c`
has no libnx in it, so the half that talks to the server compiles for a
desktop and can be driven against a real one.

---

## Still only a person can do it

- Try the on-screen pad in the hand. A layout is not something
  arithmetic settles, and the last three rounds of it were fixed from
  photographs.
- A 3DS actually asking for a Mii, and a Wii U actually asking for text.
  The path is tested end to end against the server's own demonstration,
  which drives it exactly as a backend does, but neither has been seen
  in front of a game.
