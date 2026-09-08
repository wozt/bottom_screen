#ifndef BOTTOM_SCREEN_PROTOCOL_H
#define BOTTOM_SCREEN_PROTOCOL_H

#include <stdint.h>

/*
 * The wire protocol between bottom_screen_server and a client -- the
 * Android app, the Switch homebrew, or the Linux test client.
 *
 * ONE definition, included by both sides, for the same reason as
 * capture2cloud's c2s_protocol.h: a protocol described in two places
 * drifts, and the failure mode is a stream that connects and then makes
 * no sense.
 *
 * Everything is little-endian. Every target is natively little-endian
 * (x86-64 host, aarch64 on Switch and Android), so nothing is swapped.
 *
 * The header carries fragmentation fields even over TCP, where they are
 * never needed -- packet_count is simply 1. Keeping one header shape
 * means the UDP transport is a change of transport, not a change of
 * protocol, and a capture from one mode stays readable in the other.
 */

#define BS_MAGIC        0x31435342u /* "BSC1" little-endian */
#define BS_VERSION      1
#define BS_DEFAULT_PORT 5090

/* Sizes are u32 and the sender never exceeds this, so a receiver can
 * reject a malformed length instead of trying to allocate it. These
 * screens are tiny -- a 854x480 keyframe is far below this. */
#define BS_MAX_PAYLOAD (1u * 1024u * 1024u)

/* --- the three machines -------------------------------------------- */

/*
 * Native bottom-screen resolutions. The 3DS values are from Azahar's
 * src/core/3ds.h: the touch screen is 320x240. The 400x240 often quoted
 * is the TOP screen, and using it here would stretch every frame.
 */
typedef enum {
    BS_CONSOLE_DS   = 1,
    BS_CONSOLE_3DS  = 2,
    BS_CONSOLE_WIIU = 3
} BsConsole;

#define BS_DS_WIDTH     256
#define BS_DS_HEIGHT    192
#define BS_3DS_WIDTH    320
#define BS_3DS_HEIGHT   240
#define BS_WIIU_WIDTH   854
#define BS_WIIU_HEIGHT  480

/*
 * And the other screen, for BS_SCREEN_TOP.
 *
 * The DS's two are the same size. The 3DS's top is the 400x240 that gets
 * quoted for the whole machine and is wrong for the touch screen. A Wii
 * U's television output is 16:9 at 1280x720, which is nearly three times
 * the pixels of the GamePad picture -- worth knowing before turning it
 * on over a phone connection.
 *
 * Advisory. Every one of these emulators renders at whatever internal
 * resolution the person chose, and the server takes the real size from
 * the source; these are what a client draws before the first frame
 * arrives.
 */
#define BS_DS_TOP_WIDTH     256
#define BS_DS_TOP_HEIGHT    192
#define BS_3DS_TOP_WIDTH    400
#define BS_3DS_TOP_HEIGHT   240
#define BS_WIIU_TOP_WIDTH   1280
#define BS_WIIU_TOP_HEIGHT  720

typedef enum {
    BS_CODEC_H264 = 1   /* both clients decode this in hardware */
} BsCodec;

/*
 * Opus for sound, where H.264's argument does not apply: there is no
 * hardware Opus decoder to court, and Opus is simply the best codec at
 * the low bitrates and short frames this needs. Android decodes it, the
 * Switch decodes it, and capture2cloud already streams it.
 */
typedef enum {
    BS_ACODEC_OPUS = 1
} BsAudioCodec;

/* --- client -> server, once, immediately after connecting ---------- */

typedef struct __attribute__((packed)) {
    uint32_t magic;      /* BS_MAGIC */
    uint8_t  version;    /* BS_VERSION */
    uint8_t  reserved[3];
} BsHello;

/* --- server -> client, once, in reply ------------------------------ */

/*
 * extradata is the H.264 SPS/PPS in Annex B. Sending it up front lets a
 * client configure its hardware decoder before the first frame arrives,
 * which MediaCodec in particular wants.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  accepted;   /* 0 = refused; the connection then closes.
                          * The only refusal today is a full server --
                          * it serves a fixed number of clients at once
                          * -- so a client may say so plainly. */
    uint8_t  console;    /* BsConsole */
    uint8_t  codec;      /* BsCodec */
    uint16_t width;      /* native, never scaled by the server */
    uint16_t height;
    uint16_t fps;
    uint16_t extradata_size;
    /* Sound. rate of 0 means the server is sending none, which is what
     * a source without audio reports -- the client then draws no volume
     * control rather than one that does nothing. */
    uint8_t  audio_codec;    /* BsAudioCodec */
    uint8_t  audio_channels;
    uint16_t audio_rate;     /* Hz; 0 = no audio on this stream */
    /* followed by extradata_size bytes of SPS/PPS */
} BsHelloAck;

/* --- framing, both directions -------------------------------------- */

typedef enum {
    BS_MSG_VIDEO = 1,  /* BsVideoHeader + one fragment of a frame */
    BS_MSG_AUDIO = 2,  /* BsAudioHeader + one encoded Opus packet */
    BS_MSG_STREAM_INFO = 3, /* BsStreamInfo; the picture changed shape */
    BS_MSG_INPUT = 16, /* BsInputEvent, client -> server */
    BS_MSG_PING  = 17, /* empty; keeps a silent connection alive */
    BS_MSG_PONG  = 18, /* empty; reply to PING */
    /*
     * Empty, client -> server: send a keyframe now.
     *
     * A client that has just rebuilt its decoder -- after a rotation,
     * say -- has no reference picture and shows nothing until the next
     * one, which at a one-second GOP is a second of black. Asking costs
     * one packet and a single extra keyframe.
     */
    BS_MSG_REQUEST_KEYFRAME = 19,

    /*
     * BsSize, client -> server: send the picture at this size.
     *
     * An emulator rendering at four times its internal resolution puts
     * 1280x960 on the wire for a screen that is 320x240, and a phone
     * showing it in a third of its display gains nothing from the extra
     * pixels but pays for all of them. This asks for less.
     *
     * A message type of its own rather than a wider BsQuality: adding
     * fields to a struct both ends already agree on would break every
     * client that had not been rebuilt, and an unknown message type is
     * something old servers already ignore.
     */
    BS_MSG_SET_SIZE = 21,

    /*
     * BsQuality, client -> server: re-encode at this bitrate.
     *
     * The person holding the phone is the one who can see whether the
     * picture is good enough and whether the link is keeping up, so the
     * choice belongs there rather than in a server flag they would have
     * to go and change on the PC.
     */
    BS_MSG_SET_QUALITY = 20,

    /*
     * BsAudioChoice, client -> server: which of a Wii U's two outputs to
     * hear.
     *
     * A Wii U game mixes for two speakers at once, the television's and
     * the GamePad's own, and they do not carry the same thing: a title
     * can put its music on one and a menu's clicks on the other. Since
     * the screen being streamed is the GamePad's, either may be the one
     * somebody wants, so the choice belongs to whoever is listening.
     *
     * Shared between clients, like the size and the bitrate: there is
     * one encoder, so there is one answer. The last client to ask wins.
     * Meaningless anywhere but a Wii U, and ignored there.
     */
    BS_MSG_SET_AUDIO_SOURCE = 22,

    /*
     * Which of the machine's two screens to send.
     *
     * Against the whole point of this project, and useful anyway. What
     * is worth watching on a Wii U is usually the television picture,
     * not the GamePad's, and somebody who wants the main screen on a
     * phone should not have to run something else to get it.
     *
     * Per client, not shared. This is the one setting that could not be
     * shared: two people watching two different screens is the entire
     * feature, and it is why a second screen costs a second encoder
     * where a second size or bitrate would not. The encoder for a screen
     * exists only while somebody is watching it, so the cost is paid by
     * whoever asked and by nobody else.
     *
     * There is no touch on the top screen, because there is no touch on
     * the top screen. Buttons and sticks work exactly as before: input
     * belongs to the machine, not to the picture you happen to be
     * looking at.
     */
    BS_MSG_SET_SCREEN = 23,

    /*
     * Server -> client, once, just after the handshake: which screens
     * this backend can actually produce.
     *
     * Sent as its own message rather than added to BsHelloAck, whose
     * layout is followed immediately by the extradata -- a client built
     * before this exists would read the SPS/PPS from the wrong offset
     * and show nothing, which is a poor way to announce a new feature. A
     * message type it does not recognise is skipped by its length, so
     * older clients simply never learn about the top screen, and that is
     * the correct outcome for them.
     */
    BS_MSG_SCREENS = 24
} BsMsgType;

/* Which picture. The bottom screen is 0 so that everything written
 * before this existed keeps meaning what it meant. */
typedef enum {
    BS_SCREEN_BOTTOM = 0,
    BS_SCREEN_TOP    = 1
} BsScreen;

#define BS_SCREEN_COUNT 2

typedef struct __attribute__((packed)) {
    uint8_t screen;      /* BsScreen */
    uint8_t reserved[3];
} BsScreenChoice;

/* A bit per BsScreen: 1 << BS_SCREEN_TOP is set when there is one. */
typedef struct __attribute__((packed)) {
    uint8_t available;
    uint8_t reserved[3];
} BsScreens;

/* What BS_MSG_SET_AUDIO_SOURCE selects. */
typedef enum {
    BS_AUDIO_BOTH = 0,   /* summed, which is what a Wii U owner hears */
    BS_AUDIO_TV   = 1,
    BS_AUDIO_PAD  = 2
} BsAudioSource;

typedef struct {
    uint8_t source;      /* BsAudioSource */
    uint8_t reserved[3];
} BsAudioChoice;

/*
 * Prefixes every message on the TCP transport. Over UDP one datagram
 * carries exactly one of these, so the length is redundant there but
 * costs 4 bytes and keeps a single parser.
 */
typedef struct __attribute__((packed)) {
    uint8_t  type;       /* BsMsgType */
    uint8_t  reserved[3];
    uint32_t payload_size;
} BsMsgHeader;

/* --- video ---------------------------------------------------------- */

#define BS_VFLAG_KEYFRAME     0x01
#define BS_VFLAG_END_OF_FRAME 0x02  /* last fragment of this frame */

/*
 * timestamp_us is the server's CLOCK_MONOTONIC reading at the moment the
 * frame was captured, not encoded. A client on the same machine can
 * subtract it from its own clock and get true end-to-end latency; across
 * machines the clocks differ, so it is only useful as a relative measure
 * unless the two are synchronised.
 */
typedef struct __attribute__((packed)) {
    uint32_t frame_id;
    uint32_t timestamp_us;
    uint16_t fragment_id;
    uint16_t fragment_count;
    uint8_t  flags;
    uint8_t  reserved[3];
} BsVideoHeader;

/* --- the stream changing shape -------------------------------------- */

/*
 * Sent whenever the picture's size changes, which is a normal event
 * rather than a fault: every one of these emulators lets the person
 * raise its internal resolution, and the bottom screen grows with it.
 *
 * Restarting the server on such a change would drop every connected
 * client for what is, from their side, someone moving a slider. So the
 * connection survives and this says what changed.
 *
 * from_frame_id is the first frame that carries the new shape. A client
 * cannot re-initialise its decoder the moment it reads this, because
 * frames already in flight are still the old size; this says exactly
 * when to switch.
 */
typedef struct __attribute__((packed)) {
    uint32_t from_frame_id;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
} BsStreamInfo;

/* --- audio ---------------------------------------------------------- */

/*
 * Sound travels as its own message type rather than riding with the
 * video.
 *
 * The two have different clocks and different tolerances: a late frame
 * is a stutter you see once, a late sample is a click you hear. Tying
 * them to one stream would make each wait for the other. Separate
 * messages, each with its own timestamp, let a client decide its own
 * synchronisation -- and let one be dropped without touching the other.
 */
typedef struct __attribute__((packed)) {
    uint32_t timestamp_us;   /* when these samples were produced */
    uint32_t sequence;       /* so a gap is visible rather than silent */
} BsAudioHeader;

/* --- quality -------------------------------------------------------- */

/*
 * Changing the bitrate means building a new encoder, which is why this
 * is a request rather than a knob: the server applies it between two
 * frames, never underneath the one being encoded, and sends a keyframe
 * straight after so the client has something to decode against.
 *
 * fps of 0 means leave it alone. Only the DS is fixed at 60; a source
 * that produces frames at its own pace ignores this entirely.
 */
typedef struct __attribute__((packed)) {
    uint32_t bitrate;    /* bits/s; 0 = let the server derive one */
    uint16_t fps;        /* 0 = unchanged */
    uint16_t reserved;
} BsQuality;

/*
 * The size to encode at, which is not necessarily the size the emulator
 * renders at.
 *
 * Zero means "whatever the source produces", and going back to zero is
 * how a client stops asking. The server announces what it settled on
 * through STREAM_INFO, so a client learns the real answer rather than
 * assuming it got what it asked for -- it may be refused, and it is
 * always shared with whoever else is watching.
 *
 * Shared, because there is one encoder for every client. That is the
 * same trade the bitrate makes, and for the same reason: a size each
 * would mean an encoder each, which is the cost the whole design exists
 * to avoid. The last client to ask wins.
 *
 * Touch keeps arriving in the announced space, so a client that asked
 * for a smaller picture sends smaller coordinates and the server scales
 * them back before the emulator ever sees them.
 */
typedef struct __attribute__((packed)) {
    uint16_t width;      /* 0 = follow the source */
    uint16_t height;
} BsSize;

/* --- input ---------------------------------------------------------- */

typedef enum {
    BS_INPUT_TOUCH_DOWN = 1,
    BS_INPUT_TOUCH_MOVE = 2,
    BS_INPUT_TOUCH_UP   = 3,
    BS_INPUT_BUTTON_DOWN = 4,
    BS_INPUT_BUTTON_UP   = 5,
    BS_INPUT_AXIS        = 6
} BsInputType;

/*
 * Buttons are abstract: the client says A, the server's backend decides
 * what A means on that machine. A client therefore needs no knowledge of
 * which emulator is running, only which console profile to draw.
 *
 * Not every button exists on every machine -- the DS has no ZL/ZR, the
 * Wii U has no SELECT in the DS sense. A backend ignores what it cannot
 * map rather than failing.
 */
typedef enum {
    BS_BTN_A = 1, BS_BTN_B, BS_BTN_X, BS_BTN_Y,
    BS_BTN_L, BS_BTN_R, BS_BTN_ZL, BS_BTN_ZR,
    BS_BTN_START, BS_BTN_SELECT,
    BS_BTN_UP, BS_BTN_DOWN, BS_BTN_LEFT, BS_BTN_RIGHT,
    BS_BTN_HOME
} BsButton;

/*
 * The tallest picture this will encode, whatever anyone asks for.
 *
 * A client asking for a multiple of the console's own screen can reach
 * absurd numbers without meaning to: four times a Wii U GamePad is
 * 3416x1920, wider than 4K for a screen that is 854x480, and the cost
 * of encoding it lands on every client at once because there is one
 * encoder. Three times the same screen is 2562x1440, which is a little
 * more than 1440p and is where this stops.
 *
 * Enforced in the encoder rather than in the clients, so a client that
 * asks anyway is reduced rather than believed.
 */
#define BS_MAX_STREAM_HEIGHT 1440

typedef enum {
    BS_AXIS_LEFT_X = 1, BS_AXIS_LEFT_Y,   /* DS: none. 3DS: circle pad */
    BS_AXIS_RIGHT_X, BS_AXIS_RIGHT_Y      /* 3DS: C-stick. Wii U: right stick */
} BsAxis;

/*
 * Touch coordinates are in the stream's own pixel space -- whatever
 * BsHelloAck announced, which is 256x192 for a DS but larger for an
 * emulator rendering at a higher internal resolution. Not client screen
 * pixels: the client converts once, and the server stays free of client
 * geometry.
 *
 * A backend that divides by the console's native size instead of the
 * announced one puts every tap wrong by exactly the resolution scale,
 * which looks like a calibration problem and is not.
 *
 * sequence lets the server drop a reordered event over UDP without
 * needing a timestamp comparison.
 */
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    uint32_t timestamp_us;
    uint8_t  type;       /* BsInputType */
    uint8_t  code;       /* BsButton or BsAxis, unused for touch */
    int16_t  x;          /* touch x, or axis value -32768..32767 */
    int16_t  y;          /* touch y */
    uint16_t reserved;
} BsInputEvent;

#endif /* BOTTOM_SCREEN_PROTOCOL_H */
