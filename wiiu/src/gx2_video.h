#ifndef BS_WIIU_GX2_VIDEO_H
#define BS_WIIU_GX2_VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Direct NV12 renderer.
 *
 * The decoder's NV12 planes are copied as-is into GPU-native R8/RG8
 * textures. There is NO CPU YUV->RGB conversion.
 */
int gx2_video_init(void);
void gx2_video_shutdown(void);

int gx2_video_update(const uint8_t *luma,
                     const uint8_t *chroma,
                     int stride,
                     int width,
                     int height);

/*
 * Draws the last uploaded frame into target_width x target_height,
 * preserving aspect ratio.
 */
int gx2_video_draw(int target_width, int target_height);

typedef struct {
    unsigned updates;
    unsigned draws;

    uint32_t copy_avg_us;
    uint32_t copy_max_us;

    uint32_t invalidate_avg_us;
    uint32_t invalidate_max_us;
} Gx2VideoStats;

void gx2_video_stats(Gx2VideoStats *out);

#ifdef __cplusplus
}
#endif

#endif
