#ifndef BOTTOM_SCREEN_DECODER_H
#define BOTTOM_SCREEN_DECODER_H

#include <stddef.h>
#include <stdint.h>

/*
 * H.264 decoding for the Linux test client.
 *
 * Frames come out as YUV420P planes, not RGB, and that is deliberate:
 * SDL can upload YUV straight to the GPU and let it do the colour
 * conversion while drawing. Converting to RGB on the CPU first would add
 * a full-frame pass per frame for a result the GPU produces for free.
 *
 * Android does not use this file -- it hands the same H.264 bytes to
 * MediaCodec. The Switch client does, because its ffmpeg port exposes
 * the console's own hardware decoder as h264_nvtegra, so asking for that
 * by name is all it takes; see bs_decoder_create.
 */

typedef struct BsDecoder BsDecoder;

typedef struct {
    const uint8_t *y, *u, *v;
    int y_stride, u_stride, v_stride;
    int width, height;
} BsDecodedFrame;

BsDecoder *bs_decoder_create(char *err, size_t errlen);
void bs_decoder_destroy(BsDecoder *dec);

/*
 * Feeds one encoded frame. Returns 1 and fills out when a picture is
 * ready, 0 when the decoder needs more data, -1 on error. The planes
 * stay valid until the next call.
 */
int bs_decoder_decode(BsDecoder *dec, const uint8_t *data, size_t size,
                      BsDecodedFrame *out);

/*
 * Which decoder actually opened.
 *
 * Worth showing rather than assuming: the difference between the
 * console's video block and a software fallback is the difference
 * between a cool machine and a hot one, and nothing else on screen would
 * tell them apart.
 */
const char *bs_decoder_name(const BsDecoder *dec);

#endif /* BOTTOM_SCREEN_DECODER_H */
