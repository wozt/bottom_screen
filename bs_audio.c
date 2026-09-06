#include "bs_audio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

struct BsAudioEncoder {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;
    int             rate;
    int             channels;
    int             frame_size;   /* samples per channel per Opus block */
    SwrContext     *swr;          /* the emulator's rate -> Opus's 48 kHz */
    int16_t        *acc;          /* accumulates a partial block */
    int             acc_frames;
    int16_t        *conv;         /* resampler output, one call's worth */
    int             conv_cap;
    int64_t         pts;
};

static void __attribute__((format(printf, 3, 4)))
set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

BsAudioEncoder *bs_audio_create(const BsAudioConfig *cfg, char *err, size_t errlen)
{
    if (!cfg || cfg->rate <= 0 || cfg->channels < 1 || cfg->channels > 2) {
        set_err(err, errlen, "invalid audio config");
        return NULL;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
    if (!codec)
        codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        set_err(err, errlen, "no Opus encoder in this ffmpeg build");
        return NULL;
    }

    BsAudioEncoder *enc = calloc(1, sizeof(*enc));
    if (!enc) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    enc->ctx = avcodec_alloc_context3(codec);
    if (!enc->ctx) {
        set_err(err, errlen, "avcodec_alloc_context3 failed");
        goto fail;
    }

    /*
     * Opus resamples internally to 48 kHz whatever it is given, but it
     * only accepts a handful of input rates. A DS runs at 32823 Hz,
     * which is not one of them, so the emulator's rate is announced to
     * the client while the encoder is fed at 48 kHz -- see the resample
     * note in bs_audio_encode.
     */
    enc->ctx->sample_rate = 48000;
    enc->ctx->sample_fmt  = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&enc->ctx->ch_layout, cfg->channels);
    enc->ctx->bit_rate = cfg->bitrate > 0 ? cfg->bitrate
                                          : (cfg->channels > 1 ? 96000 : 64000);

    /* Low delay matters more than the last decibel: this is sound you
     * are acting on, not listening to. */
    av_opt_set(enc->ctx->priv_data, "application", "lowdelay", 0);
    av_opt_set(enc->ctx->priv_data, "frame_duration", "20", 0);

    if (avcodec_open2(enc->ctx, codec, NULL) < 0) {
        set_err(err, errlen, "avcodec_open2 failed for Opus");
        goto fail;
    }

    enc->rate       = cfg->rate;
    enc->channels   = cfg->channels;
    enc->frame_size = enc->ctx->frame_size > 0 ? enc->ctx->frame_size : 960;

    enc->frame = av_frame_alloc();
    enc->pkt   = av_packet_alloc();
    if (!enc->frame || !enc->pkt) {
        set_err(err, errlen, "frame/packet allocation failed");
        goto fail;
    }
    enc->frame->format      = enc->ctx->sample_fmt;
    enc->frame->nb_samples  = enc->frame_size;
    av_channel_layout_copy(&enc->frame->ch_layout, &enc->ctx->ch_layout);
    if (av_frame_get_buffer(enc->frame, 0) < 0) {
        set_err(err, errlen, "av_frame_get_buffer failed");
        goto fail;
    }

    enc->acc = calloc((size_t)enc->frame_size * cfg->channels, sizeof(int16_t));
    if (!enc->acc) {
        set_err(err, errlen, "out of memory");
        goto fail;
    }

    /*
     * Opus accepts 8, 12, 16, 24 or 48 kHz and nothing else, and a 3DS
     * DSP runs at 32728 -- not merely a different rate but not a round
     * one. So something has to resample, and swresample does it
     * properly rather than by repeating samples, which is what the first
     * version did and what made it audibly rough.
     */
    if (cfg->rate != 48000) {
        enc->swr = swr_alloc();
        if (!enc->swr) {
            set_err(err, errlen, "swr_alloc failed");
            goto fail;
        }
        av_opt_set_chlayout(enc->swr, "in_chlayout",  &enc->ctx->ch_layout, 0);
        av_opt_set_chlayout(enc->swr, "out_chlayout", &enc->ctx->ch_layout, 0);
        av_opt_set_int(enc->swr, "in_sample_rate",  cfg->rate, 0);
        av_opt_set_int(enc->swr, "out_sample_rate", 48000, 0);
        av_opt_set_sample_fmt(enc->swr, "in_sample_fmt",  AV_SAMPLE_FMT_S16, 0);
        av_opt_set_sample_fmt(enc->swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
        if (swr_init(enc->swr) < 0) {
            set_err(err, errlen, "swr_init failed for %d Hz", cfg->rate);
            goto fail;
        }
    }
    return enc;

fail:
    bs_audio_destroy(enc);
    return NULL;
}

void bs_audio_destroy(BsAudioEncoder *enc)
{
    if (!enc)
        return;
    free(enc->acc);
    free(enc->conv);
    if (enc->swr) swr_free(&enc->swr);
    if (enc->pkt)   av_packet_free(&enc->pkt);
    if (enc->frame) av_frame_free(&enc->frame);
    if (enc->ctx)   avcodec_free_context(&enc->ctx);
    free(enc);
}

static int emit(BsAudioEncoder *enc, BsAudioOutput cb, void *user)
{
    if (av_frame_make_writable(enc->frame) < 0)
        return -1;

    memcpy(enc->frame->data[0], enc->acc,
           (size_t)enc->frame_size * enc->channels * sizeof(int16_t));
    enc->frame->pts = enc->pts;
    enc->pts += enc->frame_size;

    if (avcodec_send_frame(enc->ctx, enc->frame) < 0)
        return -1;

    for (;;) {
        int ret = avcodec_receive_packet(enc->ctx, enc->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return -1;
        if (cb)
            cb(enc->pkt->data, (size_t)enc->pkt->size, user);
        av_packet_unref(enc->pkt);
    }
    return 0;
}

int bs_audio_encode(BsAudioEncoder *enc, const int16_t *samples, int frames,
                    BsAudioOutput cb, void *user)
{
    if (!enc || !samples || frames <= 0)
        return -1;

    const int16_t *in = samples;
    int in_frames = frames;

    if (enc->swr) {
        /* Room for the worst case plus what the resampler is still
         * holding from the previous call. */
        const int want = (int)av_rescale_rnd(
            swr_get_delay(enc->swr, enc->rate) + frames,
            48000, enc->rate, AV_ROUND_UP) + 32;
        if (want > enc->conv_cap) {
            int16_t *grown = realloc(enc->conv,
                                     (size_t)want * enc->channels * sizeof(int16_t));
            if (!grown)
                return -1;
            enc->conv = grown;
            enc->conv_cap = want;
        }
        uint8_t *out_planes[1] = { (uint8_t *)enc->conv };
        const uint8_t *in_planes[1] = { (const uint8_t *)samples };
        const int got = swr_convert(enc->swr, out_planes, want, in_planes, frames);
        if (got < 0)
            return -1;
        in = enc->conv;
        in_frames = got;
    }

    for (int i = 0; i < in_frames; i++) {
        for (int c = 0; c < enc->channels; c++)
            enc->acc[(size_t)enc->acc_frames * enc->channels + c] =
                in[(size_t)i * enc->channels + c];
        enc->acc_frames++;
        if (enc->acc_frames >= enc->frame_size) {
            if (emit(enc, cb, user) < 0)
                return -1;
            enc->acc_frames = 0;
        }
    }
    return 0;
}

int bs_audio_rate(const BsAudioEncoder *enc) { return enc ? enc->rate : 0; }
int bs_audio_channels(const BsAudioEncoder *enc) { return enc ? enc->channels : 0; }
