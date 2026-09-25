#ifndef BS_WIIU_VIDEO_H
#define BS_WIIU_VIDEO_H

#include <stdint.h>

/*
 * The console's own H.264 decoder.
 *
 * This is the whole reason a client can run here at all: a 1.24 GHz
 * PowerPC cannot decode 720p60 in software, and the console has a
 * hardware decoder sitting idle. It takes Annex B in and gives NV12
 * back -- one plane of luma, then one plane of chroma pairs, with a
 * stride that is NOT the width.
 *
 * The host sends one complete access unit per message, so there is no
 * reassembly here: what arrives is what is submitted.
 */

typedef struct {
    const uint8_t *luma;     /* Y, `stride` bytes per line */
    const uint8_t *chroma;   /* interleaved Cb/Cr, `stride` bytes per line,
                              * half as many lines */
    int            stride;   /* the decoder's `nextLine`, not the width */
    int            width;
    int            height;
} VideoFrame;

/*
 * Reserves everything the decoder needs for pictures up to this size.
 *
 * Asked for up front because the amount is a function of the size and
 * the console will not grow it later: a stream that turns out to be
 * bigger than this is refused rather than crashing halfway through a
 * frame. Returns 0, or -1 with `why` set.
 */
int  video_init(int max_width, int max_height, char *why, unsigned why_size);
void video_exit(void);

/*
 * Decodes one access unit. Returns 1 when a picture came out, 0 when the
 * decoder took the data but has nothing to show yet (it does that for a
 * stream that has not reached a recovery point), and -1 on a real error.
 *
 * The frame points into the decoder's own buffer and is valid until the
 * next call.
 */
int  video_decode(const uint8_t *annexb, uint32_t size, VideoFrame *out);

/* Throws away decoder state, for a stream that has restarted. The next
 * submission has to begin at a recovery point. */
void video_flush(void);

/* How many pictures came out, and how many submissions produced none.
 * There is no shell on this console: without these, "nothing on screen"
 * cannot be told apart from "nothing arriving". */
void video_stats(unsigned *decoded, unsigned *empty, unsigned *errors);

typedef struct {
    unsigned submitted;
    unsigned decoded;
    unsigned empty;
    unsigned errors;

    uint32_t decode_avg_us;
    uint32_t decode_max_us;

    uint32_t execute_avg_us;
    uint32_t execute_max_us;

    uint32_t invalidate_avg_us;
    uint32_t invalidate_max_us;
} VideoStats;

void video_stats_ex(VideoStats *out);

#endif /* BS_WIIU_VIDEO_H */
