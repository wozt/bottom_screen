#include "video.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/time.h>
#include <h264/decode.h>
#include <whb/log.h>

/*
 * Wii U H264DEC.
 *
 * Important details established by existing Wii U decoders such as
 * Moonlight:
 *
 *  - decoder buffers must be explicitly aligned;
 *  - the compressed bitstream is copied into aligned memory;
 *  - H264DECExecute() does NOT necessarily return literal zero on
 *    success. Low-byte status values such as 0xE4 are normal.
 */
#define DEC_ALIGN 0x400
#define FRAME_BUFFERS 3

#define DEC_PROFILE 100
#define DEC_LEVEL   40

static void    *g_mem;
static uint32_t g_mem_size;

static void    *g_fb[FRAME_BUFFERS];
static uint32_t g_fb_size;
static unsigned g_fb_index;

static uint8_t *g_bitstream;
static uint32_t g_bitstream_cap;

static int g_open;

static unsigned g_submitted;
static unsigned g_decoded;
static unsigned g_empty;
static unsigned g_errors;

static uint64_t g_decode_us_total;
static uint64_t g_execute_us_total;
static uint64_t g_invalidate_us_total;

static uint32_t g_decode_us_max;
static uint32_t g_execute_us_max;
static uint32_t g_invalidate_us_max;

static H264DecodeResult g_last;
static int g_have_last;

static int g_logged_execute;
static int g_logged_set_error;
static int g_logged_execute_error;

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}


static uint32_t elapsed_us(OSTime start)
{
    return (uint32_t)OSTicksToMicroseconds(
        OSGetSystemTime() - start);
}

static void record_decode_time(OSTime start)
{
    const uint32_t us = elapsed_us(start);

    g_decode_us_total += us;

    if (us > g_decode_us_max) {
        g_decode_us_max = us;
    }
}

static void on_frame_output(H264DecodeOutput *output)
{
    if (!output || output->frameCount <= 0 || !output->decodeResults) {
        return;
    }

    H264DecodeResult *r = output->decodeResults[output->frameCount - 1];
    if (!r) {
        return;
    }

    memcpy(&g_last, r, sizeof(g_last));
    g_have_last = 1;
}

int video_init(int max_width, int max_height, char *why, unsigned why_size)
{
    H264Error err;

    video_exit();

    err = H264DECMemoryRequirement(
        DEC_PROFILE,
        DEC_LEVEL,
        max_width,
        max_height,
        &g_mem_size);

    if (err != H264_ERROR_OK || g_mem_size == 0) {
        snprintf(why, why_size,
                 "decoder refused %dx%d (error 0x%08x)",
                 max_width, max_height, (unsigned)err);
        return -1;
    }

    g_mem = memalign(DEC_ALIGN, g_mem_size);
    if (!g_mem) {
        snprintf(why, why_size,
                 "no room for decoder (%u bytes)",
                 (unsigned)g_mem_size);
        return -1;
    }

    memset(g_mem, 0, g_mem_size);

    err = H264DECCheckMemSegmentation(g_mem, g_mem_size);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size,
                 "decoder memory segmentation error 0x%08x",
                 (unsigned)err);
        video_exit();
        return -1;
    }

    /*
     * NV12 framebuffer.
     *
     * H264DEC works in macroblock rows and the pitch is rounded to
     * 256 bytes. Use a 0x400 aligned allocation, matching established
     * Wii U H264DEC users.
     */
    const uint32_t pitch =
        (uint32_t)(max_width + 0xff) & ~0xffu;

    const uint32_t coded_height =
        (uint32_t)(max_height + 0x0f) & ~0x0fu;

    g_fb_size = align_up(
        pitch * coded_height * 3 / 2 + 4096,
        DEC_ALIGN);

    for (unsigned i = 0; i < FRAME_BUFFERS; ++i) {
        g_fb[i] = memalign(DEC_ALIGN, g_fb_size);

        if (!g_fb[i]) {
            snprintf(why, why_size,
                     "no room for framebuffer %u (%u bytes)",
                     i,
                     (unsigned)g_fb_size);
            video_exit();
            return -1;
        }

        memset(g_fb[i], 0, g_fb_size);

        err = H264DECCheckMemSegmentation(
            g_fb[i],
            g_fb_size);

        if (err != H264_ERROR_OK) {
            snprintf(why, why_size,
                     "framebuffer %u segmentation error 0x%08x",
                     i,
                     (unsigned)err);
            video_exit();
            return -1;
        }
    }

    g_fb_index = 0;

    err = H264DECInitParam(g_mem_size, g_mem);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size,
                 "H264DECInitParam 0x%08x", (unsigned)err);
        video_exit();
        return -1;
    }

    err = H264DECSetParam_FPTR_OUTPUT(g_mem, on_frame_output);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size,
                 "H264DEC callback 0x%08x", (unsigned)err);
        video_exit();
        return -1;
    }

    err = H264DECSetParam_OUTPUT_PER_FRAME(g_mem, 1);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size,
                 "H264DEC output mode 0x%08x", (unsigned)err);
        video_exit();
        return -1;
    }

    err = H264DECOpen(g_mem);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size,
                 "H264DECOpen 0x%08x", (unsigned)err);
        video_exit();
        return -1;
    }

    g_open = 1;

    err = H264DECBegin(g_mem);
    if (err != H264_ERROR_OK) {
        snprintf(why, why_size,
                 "H264DECBegin 0x%08x", (unsigned)err);
        video_exit();
        return -1;
    }

    g_submitted = 0;
    g_decoded = 0;
    g_empty = 0;
    g_errors = 0;

    g_decode_us_total = 0;
    g_execute_us_total = 0;
    g_invalidate_us_total = 0;

    g_decode_us_max = 0;
    g_execute_us_max = 0;
    g_invalidate_us_max = 0;
    g_logged_execute = 0;
    g_logged_set_error = 0;
    g_logged_execute_error = 0;
    g_have_last = 0;

    WHBLogPrintf(
        "video: H264DEC ready, work=%u KiB fb=%u KiB",
        (unsigned)(g_mem_size / 1024),
        (unsigned)(g_fb_size / 1024));

    return 0;
}

void video_exit(void)
{
    if (g_open) {
        /*
         * Do NOT flush while terminating the application.
         *
         * Flush is useful when keeping the decoder alive and changing
         * streams, but on HOME -> Quitter we are throwing every pending
         * picture away anyway.
         *
         * On real hardware H264DECFlush() can stall here after ProcUI
         * has released the foreground. H264DECEnd()+Close() is enough
         * for complete decoder destruction.
         */
        WHBLogPrintf("video exit: H264DECEnd begin");
        H264Error end_rc = H264DECEnd(g_mem);
        WHBLogPrintf("video exit: H264DECEnd rc=0x%08x",
                     (unsigned)end_rc);

        WHBLogPrintf("video exit: H264DECClose begin");
        H264Error close_rc = H264DECClose(g_mem);
        WHBLogPrintf("video exit: H264DECClose rc=0x%08x",
                     (unsigned)close_rc);

        g_open = 0;
    }

    free(g_bitstream);
    g_bitstream = NULL;
    g_bitstream_cap = 0;

    for (unsigned i = 0; i < FRAME_BUFFERS; ++i) {
        free(g_fb[i]);
        g_fb[i] = NULL;
    }

    g_fb_index = 0;
    g_fb_size = 0;

    free(g_mem);
    g_mem = NULL;
    g_mem_size = 0;

    g_have_last = 0;
}

int video_decode(const uint8_t *annexb, uint32_t size, VideoFrame *out)
{
    if (!g_open || !annexb || !size || !out) {
        return -1;
    }

    const OSTime decode_start = OSGetSystemTime();
    g_submitted++;

    /*
     * Do not hand malloc/realloc/network memory directly to H264DEC.
     * Copy every AU into a DMA-safe aligned buffer first.
     */
    if (size > g_bitstream_cap) {
        uint32_t new_cap = align_up(size + size / 2 + 4096, DEC_ALIGN);

        uint8_t *new_buffer =
            (uint8_t *)memalign(DEC_ALIGN, new_cap);

        if (!new_buffer) {
            g_errors++;
            record_decode_time(decode_start);
            return -1;
        }

        free(g_bitstream);
        g_bitstream = new_buffer;
        g_bitstream_cap = new_cap;
    }

    memcpy(g_bitstream, annexb, size);
    DCFlushRange(g_bitstream, size);

    g_have_last = 0;

    H264Error err =
        H264DECSetBitstream(
            g_mem,
            g_bitstream,
            size,
            0.0);

    if (err != H264_ERROR_OK) {
        g_errors++;

        if (!g_logged_set_error) {
            WHBLogPrintf(
                "video: H264DECSetBitstream failed rc=0x%08x size=%u",
                (unsigned)err,
                (unsigned)size);
            g_logged_set_error = 1;
        }

        record_decode_time(decode_start);
        return -1;
    }

    /*
     * Decode directly into one of two buffers that GX2 can consume.
     *
     * No framebuffer -> CPU copy -> texture anymore.
     */
    void *decode_fb = g_fb[g_fb_index];

    g_fb_index =
        (g_fb_index + 1) %
        FRAME_BUFFERS;

    const OSTime execute_start =
        OSGetSystemTime();

    err = H264DECExecute(
        g_mem,
        decode_fb);

    const uint32_t execute_us =
        elapsed_us(execute_start);

    g_execute_us_total += execute_us;

    if (execute_us > g_execute_us_max) {
        g_execute_us_max = execute_us;
    }

    /*
     * CRITICAL:
     *
     * H264DECExecute() uses the low byte for successful status values.
     * Moonlight-WiiU does the same test:
     *
     *     if ((res & ~0xff) != 0) -> actual error
     *
     * 0xE4 is therefore NOT a decode failure.
     */
    if (((uint32_t)err & ~0xffu) != 0) {
        g_errors++;

        if (!g_logged_execute_error) {
            WHBLogPrintf(
                "video: H264DECExecute REAL error rc=0x%08x",
                (unsigned)err);
            g_logged_execute_error = 1;
        }

        record_decode_time(decode_start);
        return -1;
    }

    if (!g_logged_execute) {
        WHBLogPrintf(
            "video: first H264DECExecute rc=0x%02x callback=%d",
            (unsigned)err & 0xffu,
            g_have_last);
        g_logged_execute = 1;
    }

    if (!g_have_last) {
        /*
         * Valid situation before an IDR/recovery point.
         */
        g_empty++;
        record_decode_time(decode_start);
        return 0;
    }

    /*
     * Do not reject g_last.status here. Existing working Wii U H264DEC
     * implementations consume the returned frame when the callback
     * supplies one; Execute's high bits are the actual failure test.
     */
    if (!g_last.framebuffer ||
        g_last.width <= 0 ||
        g_last.height <= 0 ||
        g_last.nextLine < g_last.width) {

        g_errors++;

        WHBLogPrintf(
            "video: bad output fb=%p %dx%d stride=%d status=0x%08x",
            g_last.framebuffer,
            g_last.width,
            g_last.height,
            g_last.nextLine,
            (unsigned)g_last.status);

        record_decode_time(decode_start);
        return -1;
    }

    /*
     * Do not pull the whole NV12 picture back into the CPU cache.
     *
     * GX2 will consume this memory directly.
     */
    out->luma =
        (const uint8_t *)g_last.framebuffer;

    out->chroma =
        out->luma +
        (size_t)g_last.nextLine *
        (size_t)g_last.height;

    out->stride = g_last.nextLine;
    out->width = g_last.width;
    out->height = g_last.height;

    g_decoded++;

    record_decode_time(decode_start);

    return 1;
}

void video_flush(void)
{
    if (!g_open) {
        return;
    }

    H264DECFlush(g_mem);
    g_have_last = 0;
}

void video_stats(unsigned *decoded, unsigned *empty, unsigned *errors)
{
    if (decoded) *decoded = g_decoded;
    if (empty)   *empty   = g_empty;
    if (errors)  *errors  = g_errors;
}

void video_stats_ex(VideoStats *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->submitted = g_submitted;
    out->decoded = g_decoded;
    out->empty = g_empty;
    out->errors = g_errors;

    if (g_submitted) {
        out->decode_avg_us =
            (uint32_t)(g_decode_us_total /
                       g_submitted);

        out->execute_avg_us =
            (uint32_t)(g_execute_us_total /
                       g_submitted);
    }

    if (g_decoded) {
        out->invalidate_avg_us =
            (uint32_t)(g_invalidate_us_total /
                       g_decoded);
    }

    out->decode_max_us = g_decode_us_max;
    out->execute_max_us = g_execute_us_max;
    out->invalidate_max_us = g_invalidate_us_max;
}
