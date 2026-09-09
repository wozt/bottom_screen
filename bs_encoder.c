#include "bs_encoder.h"
#include "bs_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
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

    /*
     * The hardware path, when one was asked for and worked.
     *
     * A GPU encoder does not take a picture out of ordinary memory: it
     * wants one in its own, so `frame` becomes a handle to something on
     * the card and `sw_frame` is the staging copy that gets uploaded to
     * it. Both null on the software path, which is every path this had
     * before.
     */
    AVBufferRef *hw_device;
    AVBufferRef *hw_frames;
    AVFrame     *sw_frame;
};

/*
 * The pixel format a GPU encoder wants to be handed.
 *
 * NV12 for all of them: it is what every hardware block on the desk
 * takes, and asking for YUV420P means the driver converts it again on
 * the way in.
 */
static int is_hardware_encoder(const char *name)
{
    return strstr(name, "_vaapi") || strstr(name, "_nvenc") ||
           strstr(name, "_qsv")   || strstr(name, "_vulkan");
}

static enum AVHWDeviceType hw_type_for(const char *name)
{
    if (strstr(name, "_vaapi"))  return AV_HWDEVICE_TYPE_VAAPI;
    if (strstr(name, "_nvenc"))  return AV_HWDEVICE_TYPE_CUDA;
    if (strstr(name, "_qsv"))    return AV_HWDEVICE_TYPE_QSV;
    if (strstr(name, "_vulkan")) return AV_HWDEVICE_TYPE_VULKAN;
    return AV_HWDEVICE_TYPE_NONE;
}

static enum AVPixelFormat hw_pixfmt_for(enum AVHWDeviceType t)
{
    switch (t) {
    case AV_HWDEVICE_TYPE_VAAPI:  return AV_PIX_FMT_VAAPI;
    case AV_HWDEVICE_TYPE_CUDA:   return AV_PIX_FMT_CUDA;
    case AV_HWDEVICE_TYPE_QSV:    return AV_PIX_FMT_QSV;
    case AV_HWDEVICE_TYPE_VULKAN: return AV_PIX_FMT_VULKAN;
    default: return AV_PIX_FMT_NONE;
    }
}

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

    /*
     * The default: the card if it has an encoder, this CPU if not.
     *
     * Tried in order, and the first that actually opens wins -- because
     * the answer is not always yes. A machine can have a GPU with no
     * encoder, or a driver that reports one and then refuses every
     * frame, and the only way to find out is to ask it. libx264 is last
     * because it always works.
     *
     * Which one was taken is announced when the server starts, so a
     * stream that looks wrong on a machine nobody has tested is one line
     * away from being explained rather than a mystery. `libx264` asked
     * for by name is still exactly that, and is the way back.
     */
    static const char *const AUTO[] = {
        "h264_vaapi",   /* AMD and Intel on Linux */
        "h264_nvenc",   /* NVIDIA */
        "h264_qsv",     /* Intel, where VAAPI is not set up */
        "libx264",      /* always */
    };

    const char *want = cfg->encoder ? cfg->encoder : "auto";
    if (!strcmp(want, "auto")) {
        for (size_t i = 0; i < sizeof(AUTO) / sizeof(AUTO[0]); i++) {
            BsEncoderConfig probe = *cfg;
            probe.encoder = AUTO[i];
            char ignored[128] = "";
            BsEncoder *e = bs_encoder_create(&probe, ignored, sizeof(ignored));
            if (e)
                return e;
        }
        set_err(err, errlen, "no encoder would open, not even libx264");
        return NULL;
    }

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

    const int hardware = is_hardware_encoder(codec->name);
    const enum AVHWDeviceType hw_type = hardware ? hw_type_for(codec->name)
                                                 : AV_HWDEVICE_TYPE_NONE;

    enc->ctx->width     = enc->out_width;
    enc->ctx->height    = enc->out_height;
    enc->ctx->pix_fmt   = hardware ? hw_pixfmt_for(hw_type) : AV_PIX_FMT_YUV420P;
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

    if (hardware) {
        /*
         * A GPU encoder needs somewhere on the card to put pictures, and
         * it will not take one out of ordinary memory. So: a device, a
         * pool of frames on it, and the context told about the pool.
         *
         * cfg->device names which card, because a machine can have more
         * than one and only one of them may have an encoder. NULL takes
         * whatever the driver offers first, which is right on the
         * ordinary machine with one GPU.
         */
        if (av_hwdevice_ctx_create(&enc->hw_device, hw_type,
                                   cfg->device, NULL, 0) < 0) {
            set_err(err, errlen, "no %s device (tried '%s')",
                    av_hwdevice_get_type_name(hw_type),
                    cfg->device ? cfg->device : "the default");
            goto fail;
        }

        enc->hw_frames = av_hwframe_ctx_alloc(enc->hw_device);
        if (!enc->hw_frames) {
            set_err(err, errlen, "cannot allocate a frame pool on the device");
            goto fail;
        }
        AVHWFramesContext *fr = (AVHWFramesContext *)enc->hw_frames->data;
        fr->format    = enc->ctx->pix_fmt;
        /* NV12 rather than YUV420P: it is what every block on a desk
         * takes, and asking for the other means the driver converts it
         * again on the way in. */
        fr->sw_format = AV_PIX_FMT_NV12;
        fr->width     = enc->out_width;
        fr->height    = enc->out_height;
        /* Enough that an upload never waits on the encoder having
         * finished with the last one, and few enough to be nothing at
         * these sizes. */
        fr->initial_pool_size = 8;
        if (av_hwframe_ctx_init(enc->hw_frames) < 0) {
            set_err(err, errlen, "cannot initialise the frame pool");
            goto fail;
        }
        enc->ctx->hw_frames_ctx = av_buffer_ref(enc->hw_frames);
        if (!enc->ctx->hw_frames_ctx) {
            set_err(err, errlen, "out of memory");
            goto fail;
        }
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
    if (hardware) {
        /*
         * Two frames on this path. `sw_frame` is where the conversion
         * lands, in ordinary memory; `frame` is a handle to a picture on
         * the card that it is uploaded into. The upload is the only
         * copy the hardware path adds, and it buys the whole encode.
         */
        enc->sw_frame = av_frame_alloc();
        if (!enc->sw_frame) {
            set_err(err, errlen, "frame allocation failed");
            goto fail;
        }
        enc->sw_frame->format = AV_PIX_FMT_NV12;
        enc->sw_frame->width  = enc->out_width;
        enc->sw_frame->height = enc->out_height;
        if (av_frame_get_buffer(enc->sw_frame, 0) < 0) {
            set_err(err, errlen, "av_frame_get_buffer failed");
            goto fail;
        }
        if (av_hwframe_get_buffer(enc->hw_frames, enc->frame, 0) < 0) {
            set_err(err, errlen, "cannot take a frame from the device pool");
            goto fail;
        }
    } else {
        enc->frame->format = enc->ctx->pix_fmt;
        enc->frame->width  = enc->ctx->width;
        enc->frame->height = enc->ctx->height;
        if (av_frame_get_buffer(enc->frame, 0) < 0) {
            set_err(err, errlen, "av_frame_get_buffer failed");
            goto fail;
        }
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
                              enc->out_width, enc->out_height,
                              hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P,
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
    if (enc->sws)      sws_freeContext(enc->sws);
    if (enc->pkt)      av_packet_free(&enc->pkt);
    if (enc->frame)    av_frame_free(&enc->frame);
    if (enc->sw_frame) av_frame_free(&enc->sw_frame);
    /* The context first: it holds a reference to the pool, and the pool
     * holds one to the device. Freeing them the other way round leaves
     * the encoder pointing at memory that has gone. */
    if (enc->ctx)      avcodec_free_context(&enc->ctx);
    if (enc->hw_frames) av_buffer_unref(&enc->hw_frames);
    if (enc->hw_device) av_buffer_unref(&enc->hw_device);
    free(enc);
}

int bs_encoder_encode(BsEncoder *enc, const uint8_t *src, int src_stride,
                      BsEncoderOutput cb, void *user)
{
    if (!enc || !src)
        return -1;

    /* The picture the conversion writes into: ordinary memory either
     * way, and on the hardware path a staging copy on its way to the
     * card rather than the thing the encoder is handed. */
    AVFrame *dst = enc->sw_frame ? enc->sw_frame : enc->frame;

    if (av_frame_make_writable(dst) < 0)
        return -1;

    const uint8_t *src_planes[4] = { src, NULL, NULL, NULL };
    int src_strides[4] = { src_stride, 0, 0, 0 };

    sws_scale(enc->sws, src_planes, src_strides, 0, enc->height,
              dst->data, dst->linesize);

    if (enc->sw_frame) {
        /* Onto the card. The handle keeps its own pool buffer, so this
         * writes into the frame the encoder already holds rather than
         * taking a new one every picture. */
        if (av_hwframe_transfer_data(enc->frame, enc->sw_frame, 0) < 0)
            return -1;
    }

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
