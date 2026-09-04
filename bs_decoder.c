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

BsDecoder *bs_decoder_create(char *err, size_t errlen)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        set_err(err, errlen, "no H.264 decoder in this ffmpeg build");
        return NULL;
    }

    BsDecoder *dec = calloc(1, sizeof(*dec));
    if (!dec) {
        set_err(err, errlen, "out of memory");
        return NULL;
    }

    dec->ctx = avcodec_alloc_context3(codec);
    if (!dec->ctx) {
        set_err(err, errlen, "avcodec_alloc_context3 failed");
        goto fail;
    }

    /* The stream is produced with tune=zerolatency and no B-frames, so
     * one packet in yields one picture out. Saying so lets the decoder
     * skip its own reordering delay instead of holding pictures back for
     * a reordering that never happens. */
    dec->ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    dec->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    dec->ctx->thread_count = 1;

    if (avcodec_open2(dec->ctx, codec, NULL) < 0) {
        set_err(err, errlen, "avcodec_open2 failed");
        goto fail;
    }

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
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    if (ret < 0)
        return -1;

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
