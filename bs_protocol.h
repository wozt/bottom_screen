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

typedef enum {
    BS_CODEC_H264 = 1   /* both clients decode this in hardware */
} BsCodec;

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
    uint8_t  accepted;   /* 0 = refused; the connection then closes */
    uint8_t  console;    /* BsConsole */
    uint8_t  codec;      /* BsCodec */
    uint16_t width;      /* native, never scaled by the server */
    uint16_t height;
    uint16_t fps;
    uint16_t extradata_size;
    /* followed by extradata_size bytes of SPS/PPS */
} BsHelloAck;

/* --- framing, both directions -------------------------------------- */

typedef enum {
    BS_MSG_VIDEO = 1,  /* BsVideoHeader + one fragment of a frame */
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
     * BsQuality, client -> server: re-encode at this bitrate.
     *
     * The person holding the phone is the one who can see whether the
     * picture is good enough and whether the link is keeping up, so the
     * choice belongs there rather than in a server flag they would have
     * to go and change on the PC.
     */
    BS_MSG_SET_QUALITY = 20
} BsMsgType;

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

typedef enum {
    BS_AXIS_LEFT_X = 1, BS_AXIS_LEFT_Y,   /* DS: none. 3DS: circle pad */
    BS_AXIS_RIGHT_X, BS_AXIS_RIGHT_Y      /* 3DS: C-stick. Wii U: right stick */
} BsAxis;

/*
 * Touch coordinates are in the console's own pixel space -- 0..255 by
 * 0..191 on DS -- not in client screen pixels. The client already knows
 * the native size from BsHelloAck, so it does the conversion once and
 * the server stays free of client geometry.
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
