#ifndef BS_WIIU_VIDEO_WORKER_H
#define BS_WIIU_VIDEO_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "video.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned submitted;
    unsigned decoded;
    unsigned dropped;
    unsigned errors;
    unsigned queue_depth;
} VideoWorkerStats;

/*
 * H264DEC runs on a worker thread.
 *
 * The main thread stays available for:
 *   recv()
 *   Opus
 *   input
 *   GX2 / SDL present
 */
int video_worker_start(char *why, size_t why_size);
void video_worker_stop(void);

/*
 * Copies one Annex-B access unit into the bounded worker queue.
 *
 *  1 = queued
 *  0 = queue full, caller must resync on an IDR
 * -1 = allocation/thread error
 */
int video_worker_submit(const uint8_t *data, uint32_t size);

/*
 * Same submission, but if a short TCP burst fills the compressed queue,
 * wait briefly for H264DEC to free a slot rather than dropping a
 * predictive frame and forcing an IDR resync.
 */
int video_worker_submit_wait(const uint8_t *data,
                             uint32_t size,
                             uint32_t timeout_us);

/*
 * Returns the newest decoded picture once.
 *
 * Older pictures are intentionally collapsed: for game streaming the
 * live edge matters more than displaying a stale queue.
 */
int video_worker_take(VideoFrame *out);

/*
 * Same as video_worker_take(), but if the decoder is currently working
 * on an AU, wait briefly for it.
 *
 * Used to move a little of the VBlank wait before rendering instead of
 * repeating the previous picture because the decoder finished a few
 * hundred microseconds too late.
 */
int video_worker_take_wait(VideoFrame *out,
                           uint32_t timeout_us);

void video_worker_stats(VideoWorkerStats *out);

#ifdef __cplusplus
}
#endif

#endif
