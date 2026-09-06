#ifndef BOTTOM_SCREEN_AUDIO_H
#define BOTTOM_SCREEN_AUDIO_H

#include <stddef.h>
#include <stdint.h>

/*
 * Opus encoding, wrapped so nothing above this file includes libavcodec.
 *
 * Opus rather than anything else because the argument that chose H.264
 * for video does not apply here: there is no hardware Opus decoder to
 * court, and at the bitrates and frame sizes this needs -- a few dozen
 * kbit/s, 20 ms at a time -- nothing else comes close. Android decodes
 * it, the Switch decodes it, and capture2cloud already streams it.
 */

typedef struct BsAudioEncoder BsAudioEncoder;

typedef struct {
    int rate;       /* Hz, as the emulator produces it */
    int channels;   /* 1 or 2 */
    int bitrate;    /* bits/s; 0 picks a sensible default */
} BsAudioConfig;

typedef void (*BsAudioOutput)(const uint8_t *data, size_t size, void *user);

BsAudioEncoder *bs_audio_create(const BsAudioConfig *cfg, char *err, size_t errlen);
void bs_audio_destroy(BsAudioEncoder *enc);

/*
 * Feeds interleaved 16-bit samples. frames is per channel, so 480
 * frames of stereo is 960 int16_t.
 *
 * Opus only encodes fixed-length blocks, so this accumulates until it
 * has a whole one and may emit zero, one or several packets per call.
 * The caller does not have to care what size the emulator hands it.
 */
int bs_audio_encode(BsAudioEncoder *enc, const int16_t *samples, int frames,
                    BsAudioOutput cb, void *user);

int bs_audio_rate(const BsAudioEncoder *enc);
int bs_audio_channels(const BsAudioEncoder *enc);

#endif /* BOTTOM_SCREEN_AUDIO_H */
