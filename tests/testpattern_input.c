#include "bs_source.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void pause_ms(int ms)
{
    struct timespec ts = {ms / 1000, ms % 1000 * 1000000L};
    nanosleep(&ts, NULL);
}

static double tone_frequency(BsSource *s, int *peak)
{
    int16_t pcm[1920];
    int samples = 0, crossings = 0, previous = 0;
    *peak = 0;
    for (int n = 0; n < 20; n++) {
        pause_ms(5);
        int got = s->take_audio(s->self, pcm, 960);
        for (int i = 0; i < got; i++) {
            if (pcm[2*i] > 0 && previous <= 0) crossings++;
            previous = pcm[2*i];
            if (abs(previous) > *peak) *peak = abs(previous);
        }
        samples += got;
    }
    assert(samples > 0);
    return 48000.0 * crossings / samples;
}

int main(void)
{
    for (int console = BS_CONSOLE_DS; console <= BS_CONSOLE_WIIU; console++) {
        BsSource *s = bs_testpattern_create(console, 60);
        assert(s);
        BsSourceInfo info;
        s->get_info(s->self, &info);
        int stride;
        const uint8_t *pixels = s->acquire(s->self, &stride, NULL);
        size_t pos = (size_t)(info.height/3) * stride + (info.width/3)*4;
        uint8_t before[4];
        memcpy(before, pixels + pos, 4);
        s->axis(s->self, BS_AXIS_LEFT_X, 32767);
        pixels = s->acquire(s->self, &stride, NULL);
        assert(memcmp(before, pixels + pos, 4));
        s->touch(s->self, BS_INPUT_TOUCH_DOWN, info.width/3, info.height/3);
        pixels = s->acquire(s->self, &stride, NULL);
        uint32_t color;
        memcpy(&color, pixels + pos, 4);
        assert(color == 0xFFFFCC40u);
        s->touch(s->self, BS_INPUT_TOUCH_MOVE, info.width/2, info.height/2);
        pixels = s->acquire(s->self, &stride, NULL);
        memcpy(&color, pixels + (info.height/2)*stride + (info.width/2)*4, 4);
        assert(color == 0xFFFFCC40u);
        s->destroy(s->self); free(s);
    }

    for (int button = 1; button <= 15; button++) {
        BsSource *s = bs_testpattern_create(BS_CONSOLE_DS, 60);
        int16_t pcm[960*2];
        assert(s->take_audio(s->self, pcm, 960) == 0);
        s->button(s->self, button, 1);
        int samples = 0, crossings = 0, previous = 0, peak = 0;
        for (int n = 0; n < 20; n++) {
            pause_ms(5);
            int got = s->take_audio(s->self, pcm, 960);
            for (int i = 0; i < got; i++) {
                assert(pcm[2*i] == pcm[2*i+1]);
                if (pcm[2*i] > 0 && previous <= 0) crossings++;
                previous = pcm[2*i];
                if (abs(previous) > peak) peak = abs(previous);
            }
            samples += got;
        }
        // The old source delivered 19200 samples in these ~100 ms calls.
        assert(samples > 4000 && samples < 8000);
        double octave = button == BS_BTN_UP ? 1 : button == BS_BTN_DOWN ? -1 : 0;
        double expected = 440.0 * pow(2.0, (button-1)/12.0 + octave) * samples/48000;
        assert(fabs(crossings - expected) < 2.0);
        assert(peak > 1000);
        s->button(s->self, button, 0);
        for (int n = 0; n < 10; n++) {
            pause_ms(5);
            s->take_audio(s->self, pcm, 960);
        }
        pause_ms(5);
        int got = s->take_audio(s->self, pcm, 960);
        for (int i = 0; i < got*2; i++) assert(abs(pcm[i]) < 2);
        s->destroy(s->self); free(s);
    }
    BsSource *s = bs_testpattern_create(BS_CONSOLE_DS, 60);
    int16_t pcm[1920];
    s->take_audio(s->self, pcm, 960);
    int peak;
    s->touch(s->self, BS_INPUT_TOUCH_DOWN, 0, 0);
    double left = tone_frequency(s, &peak);
    assert(peak > 7000 && fabs(left - 440) < 20);
    s->touch(s->self, BS_INPUT_TOUCH_MOVE, BS_DS_WIDTH-1, BS_DS_HEIGHT-1);
    double right = tone_frequency(s, &peak);
    assert(right > 3.8*left && right < 4.2*left);
    s->touch(s->self, BS_INPUT_TOUCH_UP, 0, 0);
    tone_frequency(s, &peak); // release envelope
    tone_frequency(s, &peak);
    assert(peak < 2);
    // All buttons plus touch must remain within the PCM gain bound.
    for (int b = 1; b <= 15; b++) s->button(s->self, b, 1);
    s->touch(s->self, BS_INPUT_TOUCH_DOWN, 100, 80);
    tone_frequency(s, &peak);
    assert(peak > 1000 && peak <= 28000);
    s->destroy(s->self); free(s);
    puts("PASS: console patterns, stick colour, touch, 15 notes, touch pitch/release, polyphony and audio pacing");
}
