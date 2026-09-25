#include "bs_source.h"
#include "bs_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

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
 *     the circle follows your finger on the phone, touch works end to
 *     end, and no emulator was needed to prove it.
 */

typedef struct {
    BsSourceInfo info;
    uint8_t     *pixels;      /* BGRA, info.width * info.height * 4 */
    int          stride;
    uint32_t     frame;
    struct timespec next;

    /* Independent voices with smoothed envelopes for buttons and touch. */
    double phase[15], envelope[15];
    double touch_phase, touch_envelope;
    int octave;
    int axes[4];
    uint64_t audio_epoch_ns, audio_frames;
    pthread_mutex_t lock;

    /* last input seen, drawn into the frame */
    int touch_x, touch_y, touching;
    uint32_t buttons;         /* bitmask over BsButton, 1 = held */

    /* Drawn differently, and without a crosshair. Two identical patterns
     * would prove the second stream exists without proving it is the
     * second screen -- and the top screen has no touch to draw. */
    int is_top;
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

    const int red = (tp->axes[0] + 32767) * 90 / 65534;
    const int green = (tp->axes[1] + 32767) * 60 / 65534;
    const int blue = (tp->axes[2] + 32767) * 90 / 65534;
    const int light = (tp->axes[3] + 32767) * 25 / 65534;

    /* Background: a gradient, so banding from a too-low bitrate shows up
     * immediately instead of hiding in flat colour. */
    for (int y = 0; y < h; y++) {
        uint32_t b = (uint32_t)(40 + 60 * y / h);
        /* Blue-grey below, warm above, so which screen you are looking
         * at is obvious in a photograph. */
        uint32_t row = tp->is_top
            ? (0xFFu << 24) | ((b / 2) << 16) | (b << 8) | (b + 70)
            : (0xFFu << 24) | (b << 16) | (b << 8) | (b + 20);
        for (int x = 0; x < w; x++) {
            uint32_t g = row + (uint32_t)((60 * x / w + green + light) << 8)
                + (uint32_t)((red + light) << 16) + blue + light;
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

    /* Last touch from the client. Never on the top screen: there is no
     * touch panel there, and the server drops what a client sends
     * anyway. A crosshair drawn here would say the opposite. */
    if (!tp->is_top) {
        const int radius = h / 16;
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x) {
                int d = x*x + y*y;
                if (d <= radius*radius && (tp->touching || d >= (radius-2)*(radius-2)))
                    put_px(tp, tp->touch_x+x, tp->touch_y+y,
                           tp->touching ? 0xFFFFCC40u : 0xFFFFFFFFu);
            }
    }

    /* Held buttons, as a row of lamps along the bottom. */
    for (int i = 0; i < 15; i++) {
        int held = (tp->buttons >> i) & 1u;
        fill_rect(tp, 4 + i * 10, h - 14, 8, 10,
                  held ? 0xFF00FFFFu : 0xFF282828u);
    }
    for (int i = -1; i <= 2; i++)
        fill_rect(tp, w - 39 + (i + 1) * 9, h - 14, 6, 10,
                  i == tp->octave ? 0xFFFFCC40u : 0xFF282828u);
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
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if ((int64_t)(now.tv_sec - tp->next.tv_sec) * 1000000000L +
        now.tv_nsec - tp->next.tv_nsec > period_ns)
        tp->next = now;
    tp->next.tv_nsec += period_ns;
    while (tp->next.tv_nsec >= 1000000000L) {
        tp->next.tv_nsec -= 1000000000L;
        tp->next.tv_sec++;
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &tp->next, NULL);

    pthread_mutex_lock(&tp->lock);
    draw(tp);
    pthread_mutex_unlock(&tp->lock);
    tp->frame++;

    if (stride)       *stride = tp->stride;
    if (timestamp_us) *timestamp_us = bs_now_us();
    return tp->pixels;
}

#define TP_AUDIO_RATE     48000
#define TP_AUDIO_CHANNELS 2

static int tp_take_audio(void *self, int16_t *out, int max_frames)
{
    TestPattern *tp = self;
    if (max_frames <= 0)
        return 0;

    /* The server drains until we return zero. Generate only elapsed real
     * time, otherwise minutes of silence queue up before a button is heard. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t ns = (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
    if (!tp->audio_epoch_ns) tp->audio_epoch_ns = ns;
    uint64_t due = (ns - tp->audio_epoch_ns) * TP_AUDIO_RATE / 1000000000u;
    if (due - tp->audio_frames > TP_AUDIO_RATE / 10)
        tp->audio_frames = due - TP_AUDIO_RATE / 100;
    uint64_t available = due - tp->audio_frames;
    if (available < (uint64_t)max_frames) max_frames = (int)available;
    if (!max_frames) return 0;
    tp->audio_frames += max_frames;

    pthread_mutex_lock(&tp->lock);
    double steps[15];
    for (int b = 0; b < 15; b++)
        steps[b] = 2.0 * 3.14159265358979 * 440.0 *
            pow(2.0, b / 12.0 + tp->octave) / TP_AUDIO_RATE;
    double touch_step = 2.0 * 3.14159265358979 * 440.0 *
        pow(2.0, 2.0 * tp->touch_x / (tp->info.width - 1) + tp->octave) / TP_AUDIO_RATE;
    double harmonic = (double)tp->touch_y / (tp->info.height - 1);
    for (int i = 0; i < max_frames; i++) {
        double sample = 0, gain_sum = 0;
        for (int b = 0; b < 15; b++) {
            // One semitone per button; a short release avoids clicks.
            double target = (tp->buttons & (1u << b)) ? 1.0 : 0.0;
            tp->envelope[b] += (target - tp->envelope[b]) * 0.004;
            sample += sin(tp->phase[b]) * tp->envelope[b] * 8000.0;
            gain_sum += tp->envelope[b];
            tp->phase[b] += steps[b];
            if (tp->phase[b] >= 2.0 * 3.14159265358979)
                tp->phase[b] -= 2.0 * 3.14159265358979;
        }
        tp->touch_envelope += ((tp->touching ? 1.0 : 0.0) - tp->touch_envelope) * 0.004;
        sample += 8000.0 * tp->touch_envelope *
            (sin(tp->touch_phase) + harmonic * 0.5 * sin(2 * tp->touch_phase)) /
            (1.0 + harmonic * 0.5);
        gain_sum += tp->touch_envelope;
        tp->touch_phase += touch_step;
        if (tp->touch_phase >= 2.0 * 3.14159265358979)
            tp->touch_phase -= 2.0 * 3.14159265358979;
        // Bound the sum without clipping the waveform of held chords.
        if (gain_sum > 3.5) sample *= 3.5 / gain_sum;
        for (int c = 0; c < TP_AUDIO_CHANNELS; c++)
            out[(size_t)i * TP_AUDIO_CHANNELS + c] = (int16_t)sample;
    }
    pthread_mutex_unlock(&tp->lock);
    return max_frames;
}

static void tp_touch(void *self, BsInputType type, int x, int y)
{
    TestPattern *tp = self;
    pthread_mutex_lock(&tp->lock);
    switch (type) {
    case BS_INPUT_TOUCH_DOWN:
    case BS_INPUT_TOUCH_MOVE:
        tp->touching = 1;
        tp->touch_x = x < 0 ? 0 : x >= tp->info.width ? tp->info.width-1 : x;
        tp->touch_y = y < 0 ? 0 : y >= tp->info.height ? tp->info.height-1 : y;
        break;
    case BS_INPUT_TOUCH_UP:
        tp->touching = 0;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&tp->lock);
}

static void tp_button(void *self, BsButton code, int pressed)
{
    TestPattern *tp = self;
    if (code < 1 || code > 15)
        return;
    pthread_mutex_lock(&tp->lock);
    uint32_t bit = 1u << (code - 1);
    if (pressed && !(tp->buttons & bit)) {
        if (code == BS_BTN_UP && tp->octave < 2) tp->octave++;
        if (code == BS_BTN_DOWN && tp->octave > -1) tp->octave--;
    }
    if (pressed) tp->buttons |= bit;
    else         tp->buttons &= ~bit;
    pthread_mutex_unlock(&tp->lock);

    /*
     * Drawn as a lamp, and named here as well. A button held long enough
     * to see is easy to check on screen; one sent by a script is a tap
     * lasting a frame, and the lamp is gone before anybody looks. This
     * is what tells a physical pad's mapping apart from silence.
     */
    if (pressed)
        fprintf(stderr, "bottom_screen: button %d down\n", (int)code);
}

static void tp_axis(void *self, BsAxis code, int value)
{
    TestPattern *tp = self;
    if (code < BS_AXIS_LEFT_X || code > BS_AXIS_RIGHT_Y) return;
    if (value > 32767) value = 32767;
    if (value < -32767) value = -32767;
    if (abs(value) < 3000) value = 0;
    pthread_mutex_lock(&tp->lock);
    tp->axes[code - BS_AXIS_LEFT_X] = value;
    pthread_mutex_unlock(&tp->lock);
}

static void tp_destroy(void *self)
{
    TestPattern *tp = self;
    if (!tp)
        return;
    pthread_mutex_destroy(&tp->lock);
    free(tp->pixels);
    free(tp);
}

static BsSource *testpattern_new(BsConsole console, int fps, int is_top);

BsSource *bs_testpattern_create(BsConsole console, int fps)
{
    return testpattern_new(console, fps, 0);
}

/*
 * The other screen, so two streams at once can be exercised without an
 * emulator -- the same argument that made the bottom one exist.
 *
 * Silent, because sound belongs to the machine rather than to a picture:
 * the server takes it from the bottom source whatever anybody is
 * watching, and a second source claiming audio would have it encoded
 * twice.
 */
BsSource *bs_testpattern_create_top(BsConsole console, int fps)
{
    return testpattern_new(console, fps, 1);
}

static BsSource *testpattern_new(BsConsole console, int fps, int is_top)
{
    int w, h;
    switch (console) {
    case BS_CONSOLE_DS:
        w = is_top ? BS_DS_TOP_WIDTH  : BS_DS_WIDTH;
        h = is_top ? BS_DS_TOP_HEIGHT : BS_DS_HEIGHT;
        break;
    case BS_CONSOLE_3DS:
        w = is_top ? BS_3DS_TOP_WIDTH  : BS_3DS_WIDTH;
        h = is_top ? BS_3DS_TOP_HEIGHT : BS_3DS_HEIGHT;
        break;
    case BS_CONSOLE_WIIU:
        w = is_top ? BS_WIIU_TOP_WIDTH  : BS_WIIU_WIDTH;
        h = is_top ? BS_WIIU_TOP_HEIGHT : BS_WIIU_HEIGHT;
        break;
    default: return NULL;
    }
    if (fps < 2)
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
    tp->is_top       = is_top;
    tp->info.audio_rate = is_top ? 0 : TP_AUDIO_RATE;
    tp->info.audio_channels = is_top ? 0 : TP_AUDIO_CHANNELS;
    tp->stride       = w * 4;
    tp->pixels       = calloc(1, (size_t)tp->stride * h);
    if (!tp->pixels) {
        free(tp); free(src);
        return NULL;
    }

    pthread_mutex_init(&tp->lock, NULL);
    tp->touch_x = w / 2;
    tp->touch_y = h / 2;

    src->self     = tp;
    src->get_info = tp_get_info;
    src->acquire  = tp_acquire;
    src->touch    = tp_touch;
    src->button   = tp_button;
    src->axis     = tp_axis;
    src->take_audio = tp_take_audio;
    src->destroy  = tp_destroy;
    return src;
}
