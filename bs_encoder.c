#include "bs_encoder.h"
#include "bs_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

struct BsEncoder {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;
    struct SwsContext *sws;
    enum AVPixelFormat src_fmt;
    int   width, height;          /* the source's size */
    int   out_width, out_height;  /* what is actually encoded */
    int64_t pts;
    int   force_keyframe;
    char  name[64];
};

static enum AVPixelFormat to_av_pixfmt(BsPixFmt f)
{
    switch (f) {
    case BS_PIXFMT_BGRA:  return AV_PIX_FMT_BGRA;
    case BS_PIXFMT_RGBA:  return AV_PIX_FMT_RGBA;
    case BS_PIXFMT_RGB24: return AV_PIX_FMT_RGB24;
    }
    return AV_PIX_FMT_NONE;
}

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

/*
 * A starting point, not a tuned value. 0.5 bits per pixel per frame is
 * generous for these resolutions; the clamp keeps a Wii U GamePad frame
 * from asking for 12 Mbit/s on a formula that was calibrated for a DS.
 * Benchmark before trusting any of it.
 */
static int default_bitrate(int w, int h, int fps)
{
    double b = (double)w * h * fps * 0.5;
    if (b < 500000.0)  b = 500000.0;
    if (b > 6000000.0) b = 6000000.0;
    return (int)b;
}

BsEncoder *bs_encoder_create(const BsEncoderConfig *cfg, char *err, size_t errlen)
{
    if (!cfg || cfg->width <= 0 || cfg->height <= 0 || cfg->fps <= 0) {
        set_err(err, errlen, "invalid encoder config");
        return NULL;
    }

    enum AVPixelFormat src_fmt = to_av_pixfmt(cfg->pixfmt);
    if (src_fmt == AV_PIX_FMT_NONE) {
        set_err(err, errlen, "unknown source pixel format %d", (int)cfg->pixfmt);
        return NULL;
    }

    const char *want = cfg->encoder ? cfg->encoder : "libx264";
    const AVCodec *codec = avcodec_find_encoder_by_name(want);
    if (!codec) {
        set_err(err, errlen, "encoder '%s' not available in this ffmpeg build", want);
        return NULL;
    }

    BsEncoder *enc = calloc(1, sizeof(*enc));
    if (!enc) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }
    enc->width  = cfg->width;
    enc->height = cfg->height;

    /*
     * The encoded size, which is the source's unless a client asked for
     * less. Rounded to even numbers because YUV420 chroma is half
     * resolution and an odd dimension has no whole answer.
     */
    int out_w = cfg->out_width  > 0 ? cfg->out_width  : cfg->width;
    int out_h = cfg->out_height > 0 ? cfg->out_height : cfg->height;

    /*
     * A ceiling nobody gets to argue with.
     *
     * Whether the number came from a client asking for a multiple of the
     * console's screen or from the emulator's own internal resolution,
     * past a point it is only cost: there is one encoder behind every
     * client, so one of them asking for four times a GamePad -- 3416 by
     * 1920 -- is paid for by all of them, and by the machine doing the
     * encoding. The shape is kept; only the size is brought down.
     */
    if (out_h > BS_MAX_STREAM_HEIGHT) {
        out_w = (int)((long long)out_w * BS_MAX_STREAM_HEIGHT / out_h);
        out_h = BS_MAX_STREAM_HEIGHT;
    }
    out_w &= ~1;
    out_h &= ~1;
    if (out_w < 16) out_w = 16;
    if (out_h < 16) out_h = 16;
    enc->out_width  = out_w;
    enc->out_height = out_h;
    enc->src_fmt = src_fmt;
    snprintf(enc->name, sizeof(enc->name), "%s", codec->name);

    enc->ctx = avcodec_alloc_context3(codec);
    if (!enc->ctx) {
        set_err(err, errlen, "avcodec_alloc_context3 failed");
        goto fail;
    }

    enc->ctx->width     = enc->out_width;
    enc->ctx->height    = enc->out_height;
    enc->ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    enc->ctx->time_base = (AVRational){1, cfg->fps};
    enc->ctx->framerate = (AVRational){cfg->fps, 1};
    enc->ctx->gop_size  = cfg->gop > 0 ? cfg->gop : cfg->fps;
    enc->ctx->bit_rate  = cfg->bitrate > 0
                        ? cfg->bitrate
                        : default_bitrate(cfg->width, cfg->height, cfg->fps);

    /* B-frames reorder output: the encoder holds a frame back to code
     * the one after it. That is a whole frame of latency for a coding
     * gain we do not need at this size. */
    enc->ctx->max_b_frames = 0;

    /* Cap the buffer at a quarter second so a scene change cannot spend
     * a second of bitrate and arrive late. */
    enc->ctx->rc_max_rate    = enc->ctx->bit_rate;
    enc->ctx->rc_buffer_size = enc->ctx->bit_rate / 4;

    /*
     * Deliberately NOT setting AV_CODEC_FLAG_GLOBAL_HEADER.
     *
     * With it, SPS/PPS live only in extradata and never appear in the
     * stream. A client that joins late -- or that lost the first
     * datagram over UDP -- then has a decoder it cannot configure and
     * shows nothing until it reconnects. Without it, x264 repeats the
     * headers before every keyframe, so any client can start decoding
     * at the next keyframe with no out-of-band step. bs_encoder_extradata
     * returns NULL as a result, and that is the intended trade.
     */

    if (!strcmp(codec->name, "libx264")) {
        av_opt_set(enc->ctx->priv_data, "preset", "ultrafast", 0);
        /* zerolatency: no frame reordering, no lookahead, slice threads
         * instead of frame threads. Without it x264 buffers frames
         * internally and one input does not yield one output. */
        av_opt_set(enc->ctx->priv_data, "tune", "zerolatency", 0);
    }

    if (avcodec_open2(enc->ctx, codec, NULL) < 0) {
        set_err(err, errlen, "avcodec_open2 failed for '%s'", codec->name);
        goto fail;
    }

    enc->frame = av_frame_alloc();
    enc->pkt   = av_packet_alloc();
    if (!enc->frame || !enc->pkt) {
        set_err(err, errlen, "frame/packet allocation failed");
        goto fail;
    }
    enc->frame->format = enc->ctx->pix_fmt;
    enc->frame->width  = enc->ctx->width;
    enc->frame->height = enc->ctx->height;
    if (av_frame_get_buffer(enc->frame, 0) < 0) {
        set_err(err, errlen, "av_frame_get_buffer failed");
        goto fail;
    }

    /*
     * Point sampling while the size is unchanged, which is the common
     * case and the cheapest correct answer for a format conversion.
     * Actually shrinking wants a filter: dropping pixels from a picture
     * that is being made smaller loses thin lines and text outright,
     * which on a menu screen is most of what is there.
     */
    const int shrinking = (enc->out_width != cfg->width ||
                           enc->out_height != cfg->height);
    enc->sws = sws_getContext(cfg->width, cfg->height, src_fmt,
                              enc->out_width, enc->out_height, AV_PIX_FMT_YUV420P,
                              shrinking ? SWS_BILINEAR : SWS_POINT,
                              NULL, NULL, NULL);
    if (!enc->sws) {
        set_err(err, errlen, "sws_getContext failed");
        goto fail;
    }

    return enc;

fail:
    bs_encoder_destroy(enc);
    return NULL;
}

void bs_encoder_out_size(const BsEncoder *enc, int *width, int *height)
{
    if (!enc) {
        if (width)  *width = 0;
        if (height) *height = 0;
        return;
    }
    if (width)  *width  = enc->out_width;
    if (height) *height = enc->out_height;
}

void bs_encoder_destroy(BsEncoder *enc)
{
    if (!enc)
        return;
    if (enc->sws)   sws_freeContext(enc->sws);
    if (enc->pkt)   av_packet_free(&enc->pkt);
    if (enc->frame) av_frame_free(&enc->frame);
    if (enc->ctx)   avcodec_free_context(&enc->ctx);
    free(enc);
}

int bs_encoder_encode(BsEncoder *enc, const uint8_t *src, int src_stride,
                      BsEncoderOutput cb, void *user)
{
    if (!enc || !src)
        return -1;

    if (av_frame_make_writable(enc->frame) < 0)
        return -1;

    const uint8_t *src_planes[4] = { src, NULL, NULL, NULL };
    int src_strides[4] = { src_stride, 0, 0, 0 };

    sws_scale(enc->sws, src_planes, src_strides, 0, enc->height,
              enc->frame->data, enc->frame->linesize);

    enc->frame->pts = enc->pts++;
    if (enc->force_keyframe) {
        enc->frame->pict_type = AV_PICTURE_TYPE_I;
        enc->force_keyframe = 0;
    } else {
        enc->frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    if (avcodec_send_frame(enc->ctx, enc->frame) < 0)
        return -1;

    for (;;) {
        int ret = avcodec_receive_packet(enc->ctx, enc->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            return -1;
        if (cb)
            cb(enc->pkt->data, (size_t)enc->pkt->size,
               (enc->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0, user);
        av_packet_unref(enc->pkt);
    }
    return 0;
}

const uint8_t *bs_encoder_extradata(const BsEncoder *enc, size_t *size)
{
    if (!enc || !enc->ctx || enc->ctx->extradata_size <= 0) {
        if (size) *size = 0;
        return NULL;
    }
    if (size) *size = (size_t)enc->ctx->extradata_size;
    return enc->ctx->extradata;
}

void bs_encoder_request_keyframe(BsEncoder *enc)
{
    if (enc)
        enc->force_keyframe = 1;
}

const char *bs_encoder_name(const BsEncoder *enc)
{
    return enc ? enc->name : "";
}
