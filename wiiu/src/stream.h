#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bs_protocol.h"

/*
 * The Wii U client's network half, on its own thread.
 *
 * Same architecture as the Switch client: a reader thread owns the
 * socket, the main loop asks for whatever has arrived. What differs is
 * where decoding happens. The Switch decodes on the reader and hands
 * over the newest picture, latest-wins; this console decodes in
 * hardware (H264DEC) from another worker, so the reader hands over the
 * ENCODED access units instead -- and those are never dropped on
 * purpose, because H.264 frames are not independent: losing one breaks
 * every frame after it until the next keyframe.
 *
 * The other difference from the Switch is invisible from here: every
 * byte on the wire goes through bs_le.h, because the PowerPC is
 * big-endian and a memcpy of a protocol struct would write every
 * multi-byte field backwards.
 *
 * Nothing in this file is Wii U-specific: it builds and runs on a
 * desktop with POSIX sockets and pthreads, which is the only way the
 * protocol half of a console client ever gets tested. The Wii U build
 * defines __WIIU__ and the threads and locks become coreinit's
 * OSThread/OSMutex instead.
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
 * Copies the oldest undelivered access unit into caller memory.
 * Returns 1 when it filled `out` and set `keyframe`, 0 when the queue
 * is empty. The SPS/PPS from the handshake is queued as the very first
 * AU, marked as a keyframe, so the decoder sees parameter sets before
 * any slice.
 *
 * H.264 Annex B, exactly as the server sent it, minus the BsVideoHeader.
 */
int stream_take_video(uint8_t *out, uint32_t cap, uint32_t *out_size,
                      int *keyframe);

/*
 * The picture changed shape: returns 1 once per change and fills w/h.
 * When this fires, the video queue has already been emptied of the old
 * geometry's frames, so the next AU taken belongs to the new size and
 * the decoder can be rebuilt immediately. The server sends a keyframe
 * after the change, so -- like the Switch client -- this one never
 * sends BS_MSG_REQUEST_KEYFRAME: there is one encoder behind every
 * client, and a request is billed to all of them.
 */
int stream_take_resize(int *w, int *h);

/* Interleaved 16-bit stereo at 48 kHz, as many frames as are ready. */
int stream_take_audio(int16_t *out, int max_frames);

void stream_send_touch(int type, int x, int y);
void stream_send_button(int code, int pressed);
void stream_send_axis(int code, int value);

/* Asks for a picture of this size; zero for both follows the source. */
void stream_send_size(int width, int height);

/* Re-encode at this bitrate; zero lets the server choose. */
void stream_send_quality(int bitrate);

/* Which of a Wii U's two audio outputs to receive; see BsAudioSource. */
void stream_send_audio_source(int source);

/* Which of the machine's two screens to receive, and which ones this
 * server has (a bit per BsScreen; bottom only until it says otherwise). */
void stream_send_screen(int screen);
int  stream_screens(void);

/* How many clients are on the given BsScreen, the server's own count. */
int  stream_watching(int screen);

/*
 * The machine's own question, when it has one. Returns its id and fills
 * `body` with the title followed by one label per choice, NUL-separated,
 * or returns 0 when nothing is being asked.
 */
uint16_t stream_take_prompt(BsPrompt *out, char *body, size_t bodylen);
void stream_send_prompt_reply(uint16_t id, int cancelled, int choice,
                              const char *text);

/* Access units handed over so far -- a stream that is running and one
 * that is merely connected look identical otherwise. */
uint32_t stream_frames(void);
const char *stream_last_error(void);
