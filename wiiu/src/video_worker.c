#include "video_worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/condition.h>
#include <coreinit/core.h>
#include <coreinit/event.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>

/*
 * H264DEC itself is comfortably faster than 60 fps (~8.4 ms measured).
 *
 * A small queue only absorbs scheduling/network jitter. It must never
 * become a latency reservoir.
 */
#define VIDEO_PACKET_SLOTS 8
#define VIDEO_WORKER_STACK_SIZE (64 * 1024)

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
} VideoPacket;

static OSThread g_thread;

static uint8_t g_stack[VIDEO_WORKER_STACK_SIZE]
    __attribute__((aligned(0x40)));

static OSMutex g_mutex;
static OSCondition g_cond;
static OSEvent g_frame_event;
static OSEvent g_space_event;

static VideoPacket g_packets[VIDEO_PACKET_SLOTS];

static unsigned g_read;
static unsigned g_write;
static unsigned g_count;

static int g_started;
static int g_stop;

static VideoFrame g_latest;
static unsigned g_latest_sequence;
static unsigned g_taken_sequence;

static unsigned g_submitted;
static unsigned g_decoded;
static unsigned g_dropped;
static unsigned g_errors;


static int worker_entry(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogPrintf("video worker: started");

    for (;;) {
        VideoPacket *packet;

        OSLockMutex(&g_mutex);

        while (!g_stop && g_count == 0) {
            OSWaitCond(&g_cond, &g_mutex);
        }

        if (g_stop) {
            OSUnlockMutex(&g_mutex);
            break;
        }

        /*
         * Do NOT decrement g_count yet.
         *
         * Keeping this slot counted means the producer cannot wrap and
         * overwrite its compressed bytes while H264DEC is consuming it.
         */
        packet = &g_packets[g_read];

        OSUnlockMutex(&g_mutex);

        VideoFrame frame;
        const int rc =
            video_decode(
                packet->data,
                packet->size,
                &frame);

        OSLockMutex(&g_mutex);

        g_read =
            (g_read + 1) %
            VIDEO_PACKET_SLOTS;

        int signal_space = 0;

        if (g_count) {
            g_count--;
            signal_space = 1;
        }

        int signal_frame = 0;

        if (rc == 1) {
            g_latest = frame;
            g_latest_sequence++;
            g_decoded++;
            signal_frame = 1;
        } else if (rc < 0) {
            g_errors++;
        }

        OSUnlockMutex(&g_mutex);

        if (signal_space) {
            OSSignalEvent(
                &g_space_event);
        }

        if (signal_frame) {
            OSSignalEvent(
                &g_frame_event);
        }
    }

    WHBLogPrintf("video worker: stopped");
    return 0;
}


int video_worker_start(char *why, size_t why_size)
{
    if (why && why_size) {
        why[0] = '\0';
    }

    if (g_started) {
        return 0;
    }

    memset(g_packets, 0, sizeof(g_packets));
    memset(&g_latest, 0, sizeof(g_latest));

    g_read = 0;
    g_write = 0;
    g_count = 0;

    g_stop = 0;

    g_latest_sequence = 0;
    g_taken_sequence = 0;

    g_submitted = 0;
    g_decoded = 0;
    g_dropped = 0;
    g_errors = 0;

    OSInitMutex(&g_mutex);
    OSInitCond(&g_cond);

    /*
     * Manual-reset so video_worker_take() can clear it while holding
     * g_mutex. That avoids losing a signal between taking one frame and
     * the decoder publishing the next.
     */
    OSInitEvent(
        &g_frame_event,
        FALSE,
        OS_EVENT_MODE_MANUAL);

    /*
     * Signalled whenever H264DEC releases one compressed-AU slot.
     *
     * Manual reset lets the producer clear it while holding g_mutex
     * immediately before waiting, so a release cannot be lost between
     * "queue is full" and the actual wait.
     */
    OSInitEvent(
        &g_space_event,
        FALSE,
        OS_EVENT_MODE_MANUAL);

    /*
     * Keep the H264 worker off:
     *
     *   - the main application's current core;
     *   - CPU2, which the AX88179 module deliberately reserves for
     *     its USB worker + lwIP.
     *
     * Normally the title's main loop is on CPU1, so this selects CPU0.
     */
    const uint32_t main_core =
        OSGetCoreId();

    const uint16_t worker_affinity =
        main_core == 0
            ? OS_THREAD_ATTRIB_AFFINITY_CPU1
            : OS_THREAD_ATTRIB_AFFINITY_CPU0;

    WHBLogPrintf(
        "video worker: main core=%u worker core=%u",
        (unsigned)main_core,
        main_core == 0 ? 1u : 0u);

    if (!OSCreateThread(
            &g_thread,
            worker_entry,
            0,
            NULL,
            g_stack + sizeof(g_stack),
            sizeof(g_stack),
            16,
            worker_affinity)) {

        if (why && why_size) {
            snprintf(
                why,
                why_size,
                "could not create H264 worker");
        }

        return -1;
    }

    g_started = 1;

    OSSetThreadName(
        &g_thread,
        "BottomScreen H264");

    OSResumeThread(&g_thread);

    return 0;
}


void video_worker_stop(void)
{
    if (!g_started) {
        return;
    }

    OSLockMutex(&g_mutex);

    g_stop = 1;
    OSSignalCond(&g_cond);

    OSUnlockMutex(&g_mutex);

    int result = 0;
    OSJoinThread(&g_thread, &result);

    for (unsigned i = 0;
         i < VIDEO_PACKET_SLOTS;
         ++i) {

        free(g_packets[i].data);

        g_packets[i].data = NULL;
        g_packets[i].size = 0;
        g_packets[i].capacity = 0;
    }

    g_read = 0;
    g_write = 0;
    g_count = 0;

    g_started = 0;
}


static int video_worker_submit_now(const uint8_t *data,
                                   uint32_t size)
{
    if (!g_started ||
        !data ||
        size == 0) {
        return -1;
    }

    OSLockMutex(&g_mutex);

    if (g_count >= VIDEO_PACKET_SLOTS) {
        /*
         * Clear while holding the same mutex the decoder uses when it
         * releases a slot. A subsequent release therefore cannot race
         * between this test and the producer's wait.
         */
        OSResetEvent(
            &g_space_event);

        OSUnlockMutex(&g_mutex);

        return 0;
    }

    VideoPacket *packet =
        &g_packets[g_write];

    if (size > packet->capacity) {
        uint32_t new_capacity =
            size + size / 2 + 4096;

        uint8_t *new_data =
            realloc(
                packet->data,
                new_capacity);

        if (!new_data) {
            g_errors++;

            OSUnlockMutex(
                &g_mutex);

            return -1;
        }

        packet->data =
            new_data;

        packet->capacity =
            new_capacity;
    }

    memcpy(
        packet->data,
        data,
        size);

    packet->size =
        size;

    g_write =
        (g_write + 1) %
        VIDEO_PACKET_SLOTS;

    g_count++;
    g_submitted++;

    OSSignalCond(
        &g_cond);

    OSUnlockMutex(
        &g_mutex);

    return 1;
}


int video_worker_submit_wait(const uint8_t *data,
                             uint32_t size,
                             uint32_t timeout_us)
{
    int rc =
        video_worker_submit_now(
            data,
            size);

    if (rc != 0) {
        return rc;
    }

    /*
     * A full queue here normally means TCP delivered several AUs at
     * once. H264DEC takes ~8.4 ms, so waiting one decoder cycle is much
     * cheaper than losing a predictive AU and then throwing away every
     * P-frame until the next IDR.
     */
    if (timeout_us &&
        OSWaitEventWithTimeout(
            &g_space_event,
            OSMicrosecondsToTicks(
                timeout_us))) {

        rc =
            video_worker_submit_now(
                data,
                size);

        if (rc != 0) {
            return rc;
        }
    }

    /*
     * Only a genuine sustained overload reaches here.
     */
    OSLockMutex(&g_mutex);
    g_dropped++;
    OSUnlockMutex(&g_mutex);

    return 0;
}


int video_worker_submit(const uint8_t *data,
                        uint32_t size)
{
    return video_worker_submit_wait(
        data,
        size,
        0);
}


int video_worker_take(VideoFrame *out)
{
    if (!g_started || !out) {
        return 0;
    }

    OSLockMutex(&g_mutex);

    if (g_taken_sequence ==
        g_latest_sequence) {

        OSUnlockMutex(&g_mutex);
        return 0;
    }

    *out = g_latest;

    g_taken_sequence =
        g_latest_sequence;

    OSResetEvent(
        &g_frame_event);

    OSUnlockMutex(&g_mutex);

    return 1;
}


int video_worker_take_wait(VideoFrame *out,
                           uint32_t timeout_us)
{
    if (video_worker_take(out)) {
        return 1;
    }

    if (!g_started ||
        !timeout_us) {
        return 0;
    }

    /*
     * g_count includes the packet currently being decoded, because its
     * queue slot is released only after H264DEC returns.
     */
    OSLockMutex(&g_mutex);

    const int decoder_busy =
        g_count != 0;

    OSUnlockMutex(&g_mutex);

    if (!decoder_busy) {
        return 0;
    }

    if (!OSWaitEventWithTimeout(
            &g_frame_event,
            OSMicrosecondsToTicks(
                timeout_us))) {
        return 0;
    }

    return video_worker_take(out);
}


void video_worker_stats(VideoWorkerStats *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (!g_started) {
        return;
    }

    OSLockMutex(&g_mutex);

    out->submitted = g_submitted;
    out->decoded = g_decoded;
    out->dropped = g_dropped;
    out->errors = g_errors;
    out->queue_depth = g_count;

    OSUnlockMutex(&g_mutex);
}
