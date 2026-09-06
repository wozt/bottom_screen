#include "bs_mailbox.h"
#include "bs_net.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BsSourceInfo info;

    pthread_mutex_t lock;
    pthread_cond_t  frame_ready;

    uint8_t *buf[2];        /* back = being written, front = handed out */
    int      stride;
    int      back;          /* index of the buffer submit writes into */
    int      pending;       /* a frame is waiting to be collected */
    int      closed;
    uint32_t timestamp_us;

    /* Input travels the other way, under the same lock. It is a handful
     * of scalars, so there is nothing to gain from a second one. */
    BsInputState input;

    /*
     * Sound, in its own ring so a slow link drops old samples instead of
     * growing without limit. Half a second is plenty: past that the
     * sound is no longer worth hearing, it is just late.
     */
    int16_t *audio;
    int      audio_cap;      /* in frames */
    int      audio_head;     /* next frame to read */
    int      audio_count;    /* frames held */
} Mailbox;

static void mb_unblock(void *self);

static void mb_get_info(void *self, BsSourceInfo *out)
{
    *out = ((Mailbox *)self)->info;
}

static const uint8_t *mb_acquire(void *self, int *stride, uint32_t *timestamp_us)
{
    Mailbox *mb = self;

    pthread_mutex_lock(&mb->lock);
    while (!mb->pending && !mb->closed)
        pthread_cond_wait(&mb->frame_ready, &mb->lock);

    if (mb->closed) {
        pthread_mutex_unlock(&mb->lock);
        return NULL;
    }

    /* Swap: what was just written becomes what the caller reads, and the
     * next submit writes into the buffer the caller has finished with. */
    int front = mb->back;
    mb->back = 1 - mb->back;
    mb->pending = 0;

    if (stride)       *stride = mb->stride;
    if (timestamp_us) *timestamp_us = mb->timestamp_us;
    const uint8_t *p = mb->buf[front];
    pthread_mutex_unlock(&mb->lock);
    return p;
}

void bs_mailbox_submit(BsSource *src, const void *pixels, int stride)
{
    if (!src || !pixels)
        return;
    Mailbox *mb = src->self;

    pthread_mutex_lock(&mb->lock);
    if (!mb->closed) {
        const uint8_t *s = pixels;
        uint8_t *d = mb->buf[mb->back];
        if (stride == mb->stride) {
            memcpy(d, s, (size_t)mb->stride * mb->info.height);
        } else {
            for (int y = 0; y < mb->info.height; y++)
                memcpy(d + (size_t)y * mb->stride,
                       s + (size_t)y * stride, (size_t)mb->stride);
        }
        mb->timestamp_us = bs_now_us();
        mb->pending = 1;
        pthread_cond_signal(&mb->frame_ready);
    }
    pthread_mutex_unlock(&mb->lock);
}

void bs_mailbox_submit_audio(BsSource *src, const int16_t *samples, int frames)
{
    if (!src || !samples || frames <= 0)
        return;
    Mailbox *mb = src->self;
    if (!mb->audio || mb->info.audio_channels <= 0)
        return;

    const int ch = mb->info.audio_channels;
    pthread_mutex_lock(&mb->lock);
    for (int i = 0; i < frames; i++) {
        if (mb->audio_count == mb->audio_cap) {
            /* Full: drop the oldest frame to make room for this one. */
            mb->audio_head = (mb->audio_head + 1) % mb->audio_cap;
            mb->audio_count--;
        }
        const int slot = (mb->audio_head + mb->audio_count) % mb->audio_cap;
        for (int c = 0; c < ch; c++)
            mb->audio[(size_t)slot * ch + c] = samples[(size_t)i * ch + c];
        mb->audio_count++;
    }
    pthread_mutex_unlock(&mb->lock);
}

static int mb_take_audio(void *self, int16_t *out, int max_frames)
{
    Mailbox *mb = self;
    if (!mb->audio || max_frames <= 0)
        return 0;

    const int ch = mb->info.audio_channels;
    pthread_mutex_lock(&mb->lock);
    int n = mb->audio_count < max_frames ? mb->audio_count : max_frames;
    for (int i = 0; i < n; i++) {
        const int slot = (mb->audio_head + i) % mb->audio_cap;
        for (int c = 0; c < ch; c++)
            out[(size_t)i * ch + c] = mb->audio[(size_t)slot * ch + c];
    }
    mb->audio_head = (mb->audio_head + n) % mb->audio_cap;
    mb->audio_count -= n;
    pthread_mutex_unlock(&mb->lock);
    return n;
}

void bs_mailbox_input(BsSource *src, BsInputState *out)
{
    if (!src || !out)
        return;
    Mailbox *mb = src->self;
    pthread_mutex_lock(&mb->lock);
    *out = mb->input;
    pthread_mutex_unlock(&mb->lock);
}

void bs_mailbox_close(BsSource *src)
{
    if (src)
        mb_unblock(src->self);
}

static void mb_touch(void *self, BsInputType type, int x, int y)
{
    Mailbox *mb = self;
    pthread_mutex_lock(&mb->lock);
    switch (type) {
    case BS_INPUT_TOUCH_DOWN:
    case BS_INPUT_TOUCH_MOVE:
        mb->input.touching = 1;
        mb->input.touch_x = x;
        mb->input.touch_y = y;
        break;
    case BS_INPUT_TOUCH_UP:
        mb->input.touching = 0;
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&mb->lock);
}

static void mb_button(void *self, BsButton code, int pressed)
{
    Mailbox *mb = self;
    if (code < 1 || code > 31)
        return;
    pthread_mutex_lock(&mb->lock);
    uint32_t bit = 1u << (code - 1);
    if (pressed) mb->input.buttons |= bit;
    else         mb->input.buttons &= ~bit;
    pthread_mutex_unlock(&mb->lock);
}

static void mb_axis(void *self, BsAxis code, int value)
{
    Mailbox *mb = self;
    if (code < 1 || code > 4)
        return;
    pthread_mutex_lock(&mb->lock);
    mb->input.axis[code - 1] = (int16_t)value;
    pthread_mutex_unlock(&mb->lock);
}

static void mb_unblock(void *self)
{
    Mailbox *mb = self;
    pthread_mutex_lock(&mb->lock);
    mb->closed = 1;
    pthread_cond_broadcast(&mb->frame_ready);
    pthread_mutex_unlock(&mb->lock);
}

static void mb_destroy(void *self)
{
    Mailbox *mb = self;
    if (!mb)
        return;
    free(mb->buf[0]);
    free(mb->buf[1]);
    free(mb->audio);
    pthread_cond_destroy(&mb->frame_ready);
    pthread_mutex_destroy(&mb->lock);
    free(mb);
}

BsSource *bs_mailbox_create(BsConsole console, int width, int height,
                            int fps, BsPixFmt pixfmt,
                            int audio_rate, int audio_channels)
{
    if (width <= 0 || height <= 0 || fps <= 0)
        return NULL;

    Mailbox  *mb  = calloc(1, sizeof(*mb));
    BsSource *src = calloc(1, sizeof(*src));
    if (!mb || !src) {
        free(mb); free(src);
        return NULL;
    }

    mb->info.width   = width;
    mb->info.height  = height;
    mb->info.fps     = fps;
    mb->info.pixfmt  = pixfmt;
    mb->info.console = console;
    mb->info.audio_rate = audio_rate;
    mb->info.audio_channels = audio_channels;
    mb->stride       = width * 4;   /* every format here is 32-bit */
    if (pixfmt == BS_PIXFMT_RGB24)
        mb->stride = width * 3;

    mb->buf[0] = calloc(1, (size_t)mb->stride * height);
    mb->buf[1] = calloc(1, (size_t)mb->stride * height);
    if (!mb->buf[0] || !mb->buf[1]) {
        free(mb->buf[0]); free(mb->buf[1]); free(mb); free(src);
        return NULL;
    }

    if (audio_rate > 0 && audio_channels > 0) {
        mb->audio_cap = audio_rate / 2;          /* half a second */
        mb->audio = calloc((size_t)mb->audio_cap * audio_channels,
                           sizeof(int16_t));
        if (!mb->audio) {
            free(mb->buf[0]); free(mb->buf[1]); free(mb); free(src);
            return NULL;
        }
    }

    pthread_mutex_init(&mb->lock, NULL);
    pthread_cond_init(&mb->frame_ready, NULL);

    src->self     = mb;
    src->get_info = mb_get_info;
    src->acquire  = mb_acquire;
    src->touch    = mb_touch;
    src->button   = mb_button;
    src->axis     = mb_axis;
    src->take_audio = mb_take_audio;
    src->unblock  = mb_unblock;
    src->destroy  = mb_destroy;
    return src;
}
