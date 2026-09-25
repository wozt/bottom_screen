#ifndef BS_WIIU_AUDIO_H
#define BS_WIIU_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int audio_init(int rate, int channels,
               char *why, size_t why_size);

void audio_exit(void);

/*
 * One decoded PCM s16le block (interleaved stereo) ready for playback.
 */
void audio_push_pcm_s16le(const uint8_t *data,
                          uint32_t size);

void audio_stats(unsigned long *packets,
                 unsigned long *failed,
                 unsigned long *dropped);

typedef struct {
    unsigned input_fps;
    unsigned device_fps;
    unsigned used_fps;
    unsigned callback_frames;
    unsigned underruns;
} AudioDiag;

void audio_diag(AudioDiag *diag);

unsigned audio_queue_ms(void);

#ifdef __cplusplus
}
#endif

#endif
