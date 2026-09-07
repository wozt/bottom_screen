#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bs_protocol.h"

/*
 * The connection, on its own thread.
 *
 * Reading and decoding happen off the main loop, which is left free to
 * draw and to read the pad. What crosses between them is one decoded
 * picture and a little sound, both latest-wins: a frame that arrived
 * while the last one was still on screen is not worth showing late, and
 * on something you are playing, late is worse than missing.
 *
 * Decoding stays on the reader's side rather than the packets being
 * handed over raw, because H.264 frames are not independent. Dropping an
 * encoded frame breaks every frame after it until the next keyframe;
 * dropping a decoded one costs exactly that one picture.
 */

typedef struct {
    int width, height;      /* of the picture, after any renegotiation */
    int console;            /* BsConsole */
    int fps;
    int audio_rate;         /* 0 when the server sends no sound */
    int audio_channels;
} StreamInfo;

/* Connects, shakes hands, and starts the reader. Returns 0 on success. */
int  stream_connect(const char *host, uint16_t port, char *err, size_t errlen);
void stream_disconnect(void);
int  stream_connected(void);

/* What the server said it is serving. Safe to call at any time. */
void stream_info(StreamInfo *out);

/*
 * Copies the newest picture into caller memory if one has arrived since
 * the last call. Returns 1 when it filled y/u/v, 0 when nothing is new.
 *
 * The caller passes buffers it owns; the sizes come from stream_info.
 */
int stream_take_frame(uint8_t *y, uint8_t *u, uint8_t *v,
                      int y_stride, int uv_stride, int width, int height);

/* Interleaved 16-bit stereo at 48 kHz, as many frames as are ready. */
int stream_take_audio(int16_t *out, int max_frames);

void stream_send_touch(int type, int x, int y);
void stream_send_button(int code, int pressed);
void stream_send_axis(int code, int value);
void stream_request_keyframe(void);

/* Counters for the corner of the screen: a stream that is running and a
 * stream that is merely connected look identical otherwise. */
uint32_t stream_frames(void);
const char *stream_decoder_name(void);
const char *stream_last_error(void);
