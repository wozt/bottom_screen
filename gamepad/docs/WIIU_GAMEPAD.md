# Streaming to a Wii U GamePad from a PC

A PC with an RTL8821CU USB adapter pairs with a Wii U GamePad from scratch — no
console involved anywhere — and streams 864×480 video at 60 fps with stereo
audio, plus buttons, sticks and touch coming back.

Everything below is measured on that bench, not inferred. Where a setting is
load-bearing, the reason it is load-bearing is stated.

## The stack

| Piece | Repo | What it does |
|---|---|---|
| `rtw88_TSF` | [wozt/rtw88_TSF](https://github.com/wozt/rtw88_TSF) | RTL8821CU driver, exposes the MAC's TSF counter |
| `drc-hostap` | [wozt/drc-hostap](https://github.com/wozt/drc-hostap) | hostapd with Nintendo's WPS and WPA2 changes, plus fresh pairing |
| `drc-x264` | [wozt/drc-x264](https://github.com/wozt/drc-x264) | x264 with DRH slicing, no slice header, planar prediction off |
| `libdrc` | [wozt/libdrc](https://github.com/wozt/libdrc) | the protocol: vstrm video, astrm audio, cmd, input |

## Hardware

- **Adapter**: Realtek RTL8821CU, USB. Any 5 GHz AP-capable NIC may work; this
  one is what the numbers below come from.
- **GamePad**: a real Wii U GamePad, paired fresh to the PC.
- **Only one radio is needed** to run the AP and stream. Capturing your own
  frames over the air needs a second one.

## Network

| Setting | Value | Why |
|---|---|---|
| Band / channel | 5 GHz, channel 48 | the GamePad is 5 GHz only |
| Regulatory | `country_code=FR`, `ieee80211d=1`, `country3=0x49` | `0x49` is indoor; without it the AP is refused on 36–48 |
| AP MAC | a Nintendo OUI, e.g. `34:af:2c:xx:xx:xx` | the GamePad filters on it |
| MTU | **1800** | audio packets are 1572 bytes of IP. At 1500 they fragment, and fragmentation is what was left of the freeze |
| PC address | `192.168.1.10/24` | libdrc hardcodes it |
| GamePad address | `192.168.1.11` | hand it out over DHCP |
| Ports | video `50120`, audio `50121`, cmd `50123`; PC listens on `50010` msg, `50022` input, `50023` cmd | |

The driver must be loaded with **`disable_ips=1`**. Otherwise the MAC powers
down when idle and every TSF read comes back `0xEAEAEAEA`, which takes the
stream's whole timebase with it.

## Pairing

The pairing AP runs SSID `WiiU<first 11 of MAC><MAC>_STA1` (32 chars); the
normal AP runs `WiiU<MAC>` (16 chars) with `ignore_broadcast_ssid=1`. The PIN is
four symbols shown as ♠=0 ♥=1 ♦=2 ♣=3 followed by `5678`.

Two things decide whether this works at all:

- **The credential must be a raw 64-hex PSK** (`wpa_psk=`), never a passphrase.
  With a passphrase the GamePad probes the right SSID and then simply never
  authenticates. This one cost days.
- **The switch from the pairing AP to the normal AP must complete in well under
  the ~4 s the GamePad waits** after M8. Kill hostapd with `SIGTERM` and wait
  for it to exit; `SIGKILL` leaves the interface in a transient state for about
  5 seconds and the window is missed.

Templates: `conf/wiiu_pair.conf` and `conf/wiiu_normal.conf` in drc-hostap.

## Encoder — the settings that cannot be changed

x264 runs in **DRH mode** (`b_drh_mode=1`), where it returns macroblock rows
instead of NAL units and emits **no slice header**. That single fact drives
everything here.

| Setting | Value | Why |
|---|---|---|
| Rate control | **CQP, QP 32, and nothing else** | with no slice header the decoder cannot learn the QP and assumes 32; drc-x264 forces `pic_init_qp=32` and `slice_qp_delta=0` to match. CRF, ABR, or simply a different CQP quantises at one QP and signals another, and the GamePad decodes noise |
| Preset | **`medium` or below** | `slow` — what libdrc shipped — makes the GamePad ask for a keyframe on *every frame* on real video. Flat synthetic content decodes fine either way, which is why it went unnoticed for a decade |
| Chunks per frame | 5 | fixed by the protocol; DRH slicing produces exactly 5 |
| Intra refresh | on, keyint 10–30 | repairs loss as a moving wave instead of a ~25-packet keyframe burst |
| B-frames, 8×8 transform, weighted pred, `PSUB16x16` | off | |
| Reference frames | 1 | |
| Resolution | 864×480 | the panel is 854 wide; feeding 854 leaves a grey band |
| Frame rate | 59.94 | the streamer latches at this rate regardless, so feed it 60 |

## Transport

Packets of one frame are **spread across the frame interval** (`DRC_TX_SPREAD_US`,
11000 by default) from a dedicated thread. Measured on a capture of a real
console, the median gap between two large packets is **3168 µs** and only
**6.5 %** leave back to back. libdrc emptied each frame in one burst, which is
what the link lost.

Pacing reads the monotonic clock. Packet **timestamps** read the TSF directly —
do not free-run a clock and slew it toward the TSF. Anchoring it around a read
that costs a USB transfer is easy to get wrong by a millisecond a second, and
while video tolerates that, the GamePad silently drops audio that arrives late.

## Measured profile

| | Real console | This bench |
|---|---|---|
| Total | 532 pkt/s, 3.72 Mbit/s | 492 pkt/s, 2.76 Mbit/s |
| Video | 209 large pkt/s, 2.87 Mbit/s | ~300 pkt/s |
| Audio | 48 kHz stereo PCM | 126 pkt/s, 1536 B payload |
| Frame rate | 59.94 | 60.0 |
| Keyframe requests | — | **0/s sustained** |
| CPU (drc_player) | — | ~30 % of one core |

Held for 98 s straight with zero keyframe requests and zero link drops.

## Running it

```sh
# video on stdin as raw RGBA 864x480, audio through a FIFO as s16 48 kHz stereo
mkfifo /tmp/adrc.fifo
DRC_STATS=1 DRC_AUDIO_FIFO=/tmp/adrc.fifo \
ffmpeg -re -i input.mp4 \
  -map 0:v:0 -vf scale=864:480 -r 60 -pix_fmt rgba -f rawvideo pipe:1 \
  -map 0:a:0 -f s16le -ar 48000 -ac 2 -y /tmp/adrc.fifo \
| ./demos/drc_player
```

One ffmpeg drives both, so audio and video stay on one clock.

**When switching streams, deauthenticate the GamePad**
(`hostapd_cli deauthenticate <mac>`). Its decoder otherwise sits on the last
frame it understood.

### Reading the telemetry

`DRC_STATS=1` prints one line a second:

```
[drc] 60 frames/s, 0 IDR/s, 300 pkts, 0 resync, spread=11000us
```

**`resync` is the number that matters.** It counts keyframe requests from the
GamePad. Zero means it is decoding cleanly. Anything near the frame rate means
it cannot decode what you are sending, and the picture is about to freeze — that
is the signature every bug in this stack eventually produced.

## Known rough edges

- **Block artifacts.** QP is pinned at 32 and cannot be moved, so quality is
  whatever that gives. Fixing this means either preprocessing the input or
  teaching drc-x264 to vary quality per macroblock through `mb_qp_delta`, which
  *is* transmitted, while leaving the slice QP at 32.
- **Acquisition takes up to ~12 s** after a reconnect before `resync` settles at
  zero.
- 3dtest, tsdraw and simpleaudio all work on a fresh pairing, so video,
  touch, input and audio are each confirmed end to end.
