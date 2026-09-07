#include "bs_decoder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

struct BsDecoder {
    AVCodecContext *ctx;
    AVFrame        *frame;
    AVPacket       *pkt;

    /* Whether the console's own video block is in use, and how many
     * packets it has swallowed without giving a picture back. */
    int hardware;
    int silent;
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

/*
 * Opens a decoder into an existing BsDecoder, replacing whatever was
 * there. Named to ask for the console's video block; NULL for whatever
 * ffmpeg would pick, which is the software one.
 */
static int open_decoder(BsDecoder *dec, const char *name, char *err, size_t errlen)
{
    const AVCodec *codec = name ? avcodec_find_decoder_by_name(name)
                                : avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        set_err(err, errlen, "no H.264 decoder in this ffmpeg build");
        return -1;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        set_err(err, errlen, "avcodec_alloc_context3 failed");
        return -1;
    }

    /* The stream is produced with tune=zerolatency and no B-frames, so
     * one packet in yields one picture out. Saying so lets the decoder
     * skip its own reordering delay instead of holding pictures back for
     * a reordering that never happens. */
    ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        set_err(err, errlen, "avcodec_open2 failed");
        avcodec_free_context(&ctx);
        return -1;
    }

    if (dec->ctx)
        avcodec_free_context(&dec->ctx);
    dec->ctx = ctx;
    return 0;
}

BsDecoder *bs_decoder_create(char *err, size_t errlen)
{
    BsDecoder *dec = calloc(1, sizeof(*dec));
    if (!dec) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    /*
     * The Switch decodes H.264 in hardware, and its ffmpeg port exposes
     * that block as a decoder named h264_nvtegra. Asking for it by name
     * is the whole of using it: everything after this is the same code
     * the desktop client runs.
     *
     * Falling back rather than failing, because a build without it is a
     * working client with a warmer console, not a broken one -- and
     * bs_decoder_decode falls back again later if the block turns out to
     * accept packets without ever returning a picture.
     */
    if (open_decoder(dec, "h264_nvtegra", NULL, 0) == 0)
        dec->hardware = 1;
    else if (open_decoder(dec, NULL, err, errlen) != 0)
        goto fail;

    dec->frame = av_frame_alloc();
    dec->pkt   = av_packet_alloc();
    if (!dec->frame || !dec->pkt) {
        set_err(err, errlen, "frame/packet allocation failed");
        goto fail;
    }
    return dec;

fail:
    bs_decoder_destroy(dec);
    return NULL;
}

void bs_decoder_destroy(BsDecoder *dec)
{
    if (!dec)
        return;
    if (dec->pkt)   av_packet_free(&dec->pkt);
    if (dec->frame) av_frame_free(&dec->frame);
    if (dec->ctx)   avcodec_free_context(&dec->ctx);
    free(dec);
}

const char *bs_decoder_name(const BsDecoder *dec)
{
    if (!dec || !dec->ctx || !dec->ctx->codec)
        return "none";
    return dec->ctx->codec->name;
}

int bs_decoder_decode(BsDecoder *dec, const uint8_t *data, size_t size,
                      BsDecodedFrame *out)
{
    if (!dec || !data || size == 0)
        return -1;

    dec->pkt->data = (uint8_t *)data;
    dec->pkt->size = (int)size;

    int ret = avcodec_send_packet(dec->ctx, dec->pkt);
    if (ret < 0)
        return -1;

    ret = avcodec_receive_frame(dec->ctx, dec->frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        /*
         * A hardware decoder that accepts everything and returns nothing.
         *
         * The console's video block opens and takes packets happily
         * under an emulator that has no such block, and then produces no
         * picture at all -- a black screen with nothing in any log. A
         * few frames of silence is normal while it fills; thirty is not,
         * and a working picture in software beats a warm one that never
         * arrives.
         */
        if (dec->hardware && ++dec->silent > 30) {
            char why[128] = "";
            if (open_decoder(dec, NULL, why, sizeof(why)) == 0)
                dec->hardware = 0;
            dec->silent = 0;
        }
        return 0;
    }
    if (ret < 0)
        return -1;
    dec->silent = 0;

    if (dec->frame->format != AV_PIX_FMT_YUV420P)
        return -1;

    if (out) {
        out->y = dec->frame->data[0];
        out->u = dec->frame->data[1];
        out->v = dec->frame->data[2];
        out->y_stride = dec->frame->linesize[0];
        out->u_stride = dec->frame->linesize[1];
        out->v_stride = dec->frame->linesize[2];
        out->width  = dec->frame->width;
        out->height = dec->frame->height;
    }
    return 1;
}
