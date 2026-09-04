#ifndef BOTTOM_SCREEN_MAILBOX_H
#define BOTTOM_SCREEN_MAILBOX_H

#include "bs_source.h"

/*
 * The adapter between an emulator that pushes frames and a server that
 * pulls them.
 *
 * The test pattern generates a frame when asked, so BsSource::acquire
 * suits it. An emulator is the other way round: its own thread finishes
 * a frame and moves on, and it cannot be asked to wait while something
 * else encodes. A mailbox sits between the two -- the emulator drops a
 * frame in and returns immediately, the server picks it up when it is
 * ready.
 *
 * Latest wins. If two frames are submitted before the server collects
 * one, the first is overwritten and lost. That is the correct behaviour
 * here: a queue would trade a dropped frame for growing latency, and on
 * a screen you are playing on, late is worse than missing.
 *
 * Submitting copies the pixels. 256x192x4 is 196 KB, about 12 MB/s at 60
 * Hz -- far cheaper than letting the emulator's thread block on an
 * encoder, and it frees the emulator's own buffer immediately.
 */

typedef struct {
    int      touching;
    int      touch_x, touch_y;
    uint32_t buttons;      /* bit (BsButton - 1) set while held */
    int16_t  axis[4];      /* indexed by BsAxis - 1 */
} BsInputState;

/* pixfmt and the size must match what the emulator actually hands over. */
BsSource *bs_mailbox_create(BsConsole console, int width, int height,
                            int fps, BsPixFmt pixfmt);

/* Called from the emulator's thread. Never blocks, never fails. */
void bs_mailbox_submit(BsSource *src, const void *pixels, int stride);

/*
 * The input the clients have sent, as a state rather than a stream of
 * events. melonDS's emu thread already reads its touch and button state
 * once per frame; handing it a state to copy matches what it does with
 * its own Qt frontend instead of fighting it.
 */
void bs_mailbox_input(BsSource *src, BsInputState *out);

/* Unblocks a server waiting in acquire, so it can shut down. */
void bs_mailbox_close(BsSource *src);

#endif /* BOTTOM_SCREEN_MAILBOX_H */
