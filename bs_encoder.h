#ifndef BOTTOM_SCREEN_ENCODER_H
#define BOTTOM_SCREEN_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/*
 * H.264 encoding, wrapped so nothing above this file includes libavcodec.
 *
 * H.264 rather than VP8 because both clients -- Android and Switch 1 --
 * decode it in hardware. At 256x192 the encode cost is negligible on any
 * modern CPU; it is the client's decode that decides whether this is
 * playable, and there the hardware path wins outright.
 */

typedef struct BsEncoder BsEncoder;

/* Layout of the pixels handed to bs_encoder_encode. Named here rather
 * than reusing libavcodec's enum so this header stays dependency-free:
 * the Switch and Android clients include the protocol and these types
 * without ever linking ffmpeg. */
typedef enum {
    BS_PIXFMT_BGRA = 1,   /* melonDS RAM framebuffers */
    BS_PIXFMT_RGBA = 2,
    BS_PIXFMT_RGB24 = 3
} BsPixFmt;

typedef struct {
    int width;          /* the source's own size */
    int height;

    /*
     * The size to encode at, when a client has asked for less than the
     * emulator renders. 0 means the source's size.
     *
     * The scaler is already there for the pixel format -- BGRA in, YUV
     * out -- so giving it a different output size costs nothing beyond
     * the resampling itself.
     */
    int out_width;
    int out_height;
    int fps;
    int bitrate;        /* bits/s; 0 picks a default from the resolution */
    int gop;            /* keyframe interval in frames; 0 = fps (one per second) */
    BsPixFmt pixfmt;
    const char *encoder;

    /*
     * Which card, for a hardware encoder. NULL takes whatever the
     * driver offers first, which is the right answer on a machine with
     * one GPU and the wrong one on a machine with two -- only one of
     * them may have an encoder, and there is no way to tell from here
     * which the driver will pick.
     *
     * A VAAPI render node ("/dev/dri/renderD128"), a CUDA index, or
     * whatever the chosen backend names its devices. Ignored by a
     * software encoder.
     */
    const char *device; /* NULL = libx264. "h264_vaapi", "h264_nvenc" to try hardware */
} BsEncoderConfig;

/* Called once per encoded packet, from inside bs_encoder_encode. */
typedef void (*BsEncoderOutput)(const uint8_t *data, size_t size,
                                int keyframe, void *user);

BsEncoder *bs_encoder_create(const BsEncoderConfig *cfg, char *err, size_t errlen);
void bs_encoder_destroy(BsEncoder *enc);

/* src points at width*height pixels in cfg->pixfmt, src_stride bytes per
 * row. Returns 0 on success, -1 on error. */
int bs_encoder_encode(BsEncoder *enc, const uint8_t *src, int src_stride,
                      BsEncoderOutput cb, void *user);

/* SPS/PPS, or NULL. See the note in bs_encoder.c about why this is
 * usually NULL and why that is deliberate. */
const uint8_t *bs_encoder_extradata(const BsEncoder *enc, size_t *size);

/*
 * The size actually being encoded, which is what the clients must be
 * told: a request can be rounded to even numbers or clamped, and a
 * client that assumed it got exactly what it asked for would put every
 * touch in the wrong place.
 */
void bs_encoder_out_size(const BsEncoder *enc, int *width, int *height);

/* Makes the next encoded frame a keyframe. A client that connects
 * mid-stream cannot decode anything until one arrives, and waiting out
 * the GOP is a second of blank screen. */
void bs_encoder_request_keyframe(BsEncoder *enc);

const char *bs_encoder_name(const BsEncoder *enc);

#endif /* BOTTOM_SCREEN_ENCODER_H */
