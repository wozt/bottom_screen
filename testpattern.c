#include "bs_source.h"
#include "bs_net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * A synthetic bottom screen, so the whole pipeline -- encode, framing,
 * transport, decode, display, and input coming back -- can be built and
 * measured before any emulator is modified.
 *
 * It draws three things on purpose:
 *
 *   - a bar sweeping left to right, so a dropped or repeated frame is
 *     visible as a stutter rather than hiding in a static image;
 *   - a block that flips colour every half second, which is what you
 *     film alongside the client's screen to measure real latency with a
 *     camera when the two are on different machines and the clocks
 *     cannot be compared;
 *   - whatever input the client last sent, drawn where it landed. That
 *     turns the test pattern into a loopback for the input path too: if
 *     the crosshair follows your finger on the phone, touch works end to
 *     end, and no emulator was needed to prove it.
 */

typedef struct {
    BsSourceInfo info;
    uint8_t     *pixels;      /* BGRA, info.width * info.height * 4 */
    int          stride;
    uint32_t     frame;
    struct timespec next;

    /* last input seen, drawn into the frame */
    int touch_x, touch_y, touching;
    uint32_t buttons;         /* bitmask over BsButton, 1 = held */
} TestPattern;

static void put_px(TestPattern *tp, int x, int y, uint32_t bgra)
{
    if (x < 0 || y < 0 || x >= tp->info.width || y >= tp->info.height)
        return;
    memcpy(tp->pixels + (size_t)y * tp->stride + (size_t)x * 4, &bgra, 4);
}

static void fill_rect(TestPattern *tp, int x0, int y0, int w, int h, uint32_t bgra)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_px(tp, x, y, bgra);
}

static void draw(TestPattern *tp)
{
    const int w = tp->info.width, h = tp->info.height;

    /* Background: a gradient, so banding from a too-low bitrate shows up
     * immediately instead of hiding in flat colour. */
    for (int y = 0; y < h; y++) {
        uint32_t b = (uint32_t)(40 + 60 * y / h);
        uint32_t row = (0xFFu << 24) | (b << 16) | (b << 8) | (b + 20);
        for (int x = 0; x < w; x++) {
            uint32_t g = row + (uint32_t)((90 * x / w) << 8);
            memcpy(tp->pixels + (size_t)y * tp->stride + (size_t)x * 4, &g, 4);
        }
    }

    /* Sweeping bar, one full pass every two seconds. */
    int bar = (int)((tp->frame % (uint32_t)(tp->info.fps * 2)) * (uint32_t)w
                    / (uint32_t)(tp->info.fps * 2));
    fill_rect(tp, bar, 0, 3, h, 0xFFFFFFFFu);

    /* Half-second blink block, top-left, for camera-based timing. */
    int on = (tp->frame / (uint32_t)(tp->info.fps / 2)) & 1u;
    fill_rect(tp, 4, 4, 24, 24, on ? 0xFF00FF00u : 0xFF202020u);

    /* Frame counter in binary, 16 blocks along the top. Coarse, but it
     * needs no font and a camera can read it back frame by frame. */
    for (int i = 0; i < 16; i++) {
        int bit = (tp->frame >> (15 - i)) & 1u;
        fill_rect(tp, 34 + i * 6, 6, 5, 8, bit ? 0xFFFFFFFFu : 0xFF303030u);
    }

    /* Last touch from the client. */
    if (tp->touching) {
        for (int d = -8; d <= 8; d++) {
            put_px(tp, tp->touch_x + d, tp->touch_y, 0xFF0000FFu);
            put_px(tp, tp->touch_x, tp->touch_y + d, 0xFF0000FFu);
        }
        fill_rect(tp, tp->touch_x - 2, tp->touch_y - 2, 5, 5, 0xFF0000FFu);
    }

    /* Held buttons, as a row of lamps along the bottom. */
    for (int i = 0; i < 15; i++) {
        int held = (tp->buttons >> i) & 1u;
        fill_rect(tp, 4 + i * 10, h - 14, 8, 10,
                  held ? 0xFF00FFFFu : 0xFF282828u);
    }
}

static void tp_get_info(void *self, BsSourceInfo *out)
{
    *out = ((TestPattern *)self)->info;
}

static const uint8_t *tp_acquire(void *self, int *stride, uint32_t *timestamp_us)
{
    TestPattern *tp = self;

    /* Absolute-deadline pacing: sleeping for a fixed interval would
     * accumulate the render time as drift and slowly fall behind. */
    long period_ns = 1000000000L / tp->info.fps;
    if (tp->next.tv_sec == 0)
        clock_gettime(CLOCK_MONOTONIC, &tp->next);
    tp->next.tv_nsec += period_ns;
    while (tp->next.tv_nsec >= 1000000000L) {
        tp->next.tv_nsec -= 1000000000L;
        tp->next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tp->next, NULL);

    draw(tp);
    tp->frame++;

    if (stride)       *stride = tp->stride;
    if (timestamp_us) *timestamp_us = bs_now_us();
    return tp->pixels;
}

static void tp_touch(void *self, BsInputType type, int x, int y)
{
    TestPattern *tp = self;
    switch (type) {
    case BS_INPUT_TOUCH_DOWN:
    case BS_INPUT_TOUCH_MOVE:
        tp->touching = 1;
        tp->touch_x = x;
        tp->touch_y = y;
        break;
    case BS_INPUT_TOUCH_UP:
        tp->touching = 0;
        break;
    default:
        break;
    }
}

static void tp_button(void *self, BsButton code, int pressed)
{
    TestPattern *tp = self;
    if (code < 1 || code > 15)
        return;
    uint32_t bit = 1u << (code - 1);
    if (pressed) tp->buttons |= bit;
    else         tp->buttons &= ~bit;
}

static void tp_axis(void *self, BsAxis code, int value)
{
    (void)self; (void)code; (void)value;   /* nothing to show yet */
}

static void tp_destroy(void *self)
{
    TestPattern *tp = self;
    if (!tp)
        return;
    free(tp->pixels);
    free(tp);
}

BsSource *bs_testpattern_create(BsConsole console, int fps)
{
    int w, h;
    switch (console) {
    case BS_CONSOLE_DS:   w = BS_DS_WIDTH;   h = BS_DS_HEIGHT;   break;
    case BS_CONSOLE_3DS:  w = BS_3DS_WIDTH;  h = BS_3DS_HEIGHT;  break;
    case BS_CONSOLE_WIIU: w = BS_WIIU_WIDTH; h = BS_WIIU_HEIGHT; break;
    default: return NULL;
    }
    if (fps <= 0)
        fps = 60;

    TestPattern *tp = calloc(1, sizeof(*tp));
    BsSource *src = calloc(1, sizeof(*src));
    if (!tp || !src) {
        free(tp); free(src);
        return NULL;
    }

    tp->info.width   = w;
    tp->info.height  = h;
    tp->info.fps     = fps;
    tp->info.pixfmt  = BS_PIXFMT_BGRA;
    tp->info.console = console;
    tp->stride       = w * 4;
    tp->pixels       = calloc(1, (size_t)tp->stride * h);
    if (!tp->pixels) {
        free(tp); free(src);
        return NULL;
    }

    src->self     = tp;
    src->get_info = tp_get_info;
    src->acquire  = tp_acquire;
    src->touch    = tp_touch;
    src->button   = tp_button;
    src->axis     = tp_axis;
    src->destroy  = tp_destroy;
    return src;
}
