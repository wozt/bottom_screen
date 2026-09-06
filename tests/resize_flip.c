/*
 * The picture changing size between two frames, repeatedly.
 *
 * This is what a renderer switched while a game is running looks like
 * from underneath: melonDS at 2x on OpenGL hands over 512x384, and the
 * moment somebody selects the software renderer the very next frame is
 * 256x192. Growing is the easy direction; shrinking is the one that
 * hurts, because a buffer still sized for the larger picture is read to
 * its old length and runs off the end of the emulator's own framebuffer.
 *
 * Built with the sanitizers, so an overrun fails here rather than
 * corrupting a heap and surfacing somewhere unrelated.
 */
#include "bs_mailbox.h"
#include "bs_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *what, int ok)
{
    printf("%-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

/*
 * Allocated exactly, never with room to spare: a buffer with slack would
 * absorb the overrun this test exists to catch.
 */
static uint8_t *exact_frame(int w, int h, uint8_t fill)
{
    uint8_t *p = malloc((size_t)w * h * 4);
    memset(p, fill, (size_t)w * h * 4);
    return p;
}

static int round_trip(BsSource *src, int w, int h, uint8_t fill)
{
    bs_mailbox_resize(src, w, h);

    uint8_t *frame = exact_frame(w, h, fill);
    bs_mailbox_submit(src, frame, w * 4);
    free(frame);

    BsSourceInfo info;
    src->get_info(src->self, &info);
    if (info.width != w || info.height != h)
        return 0;

    int stride = 0;
    uint32_t ts = 0;
    const uint8_t *got = src->acquire(src->self, &stride, &ts);
    if (!got || stride < w * 4)
        return 0;

    /* The corners, so a stride mistake shows up rather than hiding in
     * the middle of a uniform buffer. */
    if (got[0] != fill)
        return 0;
    if (got[(size_t)(h - 1) * stride + (size_t)(w - 1) * 4] != fill)
        return 0;
    return 1;
}

int main(void)
{
    BsSource *src = bs_mailbox_create(BS_CONSOLE_DS, 256, 192, 60,
                                      BS_PIXFMT_BGRA, 0, 0);
    if (!src) {
        fprintf(stderr, "no source\n");
        return 1;
    }

    check("256x192, the software renderer", round_trip(src, 256, 192, 0x11));
    check("512x384, OpenGL at 2x", round_trip(src, 512, 384, 0x22));
    check("256x192 again, switched back mid-game",
          round_trip(src, 256, 192, 0x33));
    check("1024x768, OpenGL at 4x", round_trip(src, 1024, 768, 0x44));
    check("256x192 once more, the largest drop", round_trip(src, 256, 192, 0x55));

    /* Several frames at the small size after a big one: the first frame
     * is not the only one that has to be right. */
    int steady = 1;
    for (int i = 0; i < 8; i++)
        steady &= round_trip(src, 256, 192, (uint8_t)(0x60 + i));
    check("steady at the small size afterwards", steady);

    bs_mailbox_close(src);
    /* destroy releases what the mailbox owns; the BsSource itself is the
     * caller's, exactly as the emulator bridges free it. */
    src->destroy(src->self);
    free(src);

    printf(failures ? "\nFAILED (%d)\n" : "\nPASS\n", failures);
    return failures ? 1 : 0;
}
