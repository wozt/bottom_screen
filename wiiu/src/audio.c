#include "audio.h"

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/core.h>
#include <coreinit/memorymap.h>
#include <coreinit/time.h>

#include <sndcore2/core.h>
#include <sndcore2/voice.h>

#include <whb/log.h>

#define AUDIO_RING_FRAMES 8192
#define AUDIO_START_MS 30

/*
 * Sanity cap on one pushed PCM block. In capture2cloud this was
 * C2S_PCM_UDP_MAX_FRAMES from c2s_protocol.h; bottom_screen carries
 * Opus instead, so the decoder upstream decides the block size, but
 * keeping the same 320-frame ceiling rejects corrupt sizes exactly
 * like the original hardware-validated code did.
 */
#define AUDIO_MAX_PUSH_FRAMES 320

#define AX_MIX_CHANNELS 6
#define AX_BUS_MASTER   0
#define AX_LEFT         0
#define AX_RIGHT        1

static AXVoice *g_voice[2];

static int16_t *g_pcm[2];

static uint32_t g_write_pos;
static uint32_t g_last_read;
static uint32_t g_count;

static int g_started;
static int g_rate = 48000;
static int g_ax_owned;

static unsigned long g_packets;
static unsigned long g_failed;
static unsigned long g_dropped;

static uint32_t g_underruns;

static uint64_t g_input_total;
static uint64_t g_device_total;
static uint64_t g_used_total;

static uint32_t g_diag_at;
static uint64_t g_diag_input_at;
static uint64_t g_diag_device_at;
static uint64_t g_diag_used_at;

static unsigned g_diag_input_fps;
static unsigned g_diag_device_fps;
static unsigned g_diag_used_fps;


/*
 * Stereo voice 0 -> left
 * Stereo voice 1 -> right
 *
 * Send to both TV and GamePad, matching the old SDL backend behaviour.
 */
static AXVoiceDeviceMixData g_stereo_mix[2][AX_MIX_CHANNELS] = {
    [0] = {
        [AX_LEFT] = {
            .bus = {
                [AX_BUS_MASTER] = {
                    .volume = 0x8000,
                    .delta = 0
                }
            }
        }
    },

    [1] = {
        [AX_RIGHT] = {
            .bus = {
                [AX_BUS_MASTER] = {
                    .volume = 0x8000,
                    .delta = 0
                }
            }
        }
    }
};


static uint32_t now_ms(void)
{
    return
        (uint32_t)
        OSTicksToMilliseconds(
            OSGetSystemTime());
}


static void *ax_alloc(size_t bytes)
{
    /*
     * AX/DSP must be able to see the physical memory.
     *
     * Same restriction used by the Wii U SDL2 backend.
     */
    for (int i = 0; i < 32; ++i) {
        void *p =
            memalign(0x40, bytes);

        if (!p) {
            return NULL;
        }

        const uint32_t phys =
            OSEffectiveToPhysical(
                (uint32_t)(uintptr_t)p) &
            0x1fffffffu;

        const uint32_t end =
            phys + (uint32_t)bytes;

        if ((end & 0xe0000000u) == 0) {
            return p;
        }

        free(p);
    }

    return NULL;
}


static void stop_voices(void)
{
    for (int ch = 0; ch < 2; ++ch) {
        if (g_voice[ch]) {
            AXSetVoiceState(
                g_voice[ch],
                AX_VOICE_STATE_STOPPED);
        }
    }

    g_started = 0;
}


static void reset_voice_src(AXVoice *voice)
{
    AXVoiceSrc src;

    memset(
        &src,
        0,
        sizeof(src));

    const float ratio =
        (float)g_rate /
        (float)AXGetInputSamplesPerSec();

    src.ratio =
        (uint32_t)(
            ratio * 65536.0f +
            0.5f);

    src.currentOffsetFrac = 0;

    memset(
        src.lastSample,
        0,
        sizeof(src.lastSample));

    AXSetVoiceSrc(
        voice,
        &src);

    AXSetVoiceSrcType(
        voice,
        AX_VOICE_SRC_TYPE_LINEAR);
}


static void audio_sync_position(void)
{
    if (!g_started ||
        !g_voice[0]) {
        return;
    }

    uint32_t current =
        AXGetVoiceCurrentOffsetEx(
            g_voice[0],
            g_pcm[0]);

    if (current >= AUDIO_RING_FRAMES) {
        current %=
            AUDIO_RING_FRAMES;
    }

    const uint32_t consumed =
        (current +
         AUDIO_RING_FRAMES -
         g_last_read) %
        AUDIO_RING_FRAMES;

    if (!consumed) {
        return;
    }

    g_device_total +=
        consumed;

    g_used_total +=
        consumed;

    /*
     * AX has reached or passed the end of valid queued PCM.
     *
     * Stop immediately and rebuild a short live buffer instead of
     * letting the DSP loop over stale ring contents.
     */
    if (consumed >= g_count) {
        g_underruns++;

        g_count = 0;

        g_write_pos =
            current;

        g_last_read =
            current;

        stop_voices();

        return;
    }

    g_count -=
        consumed;

    g_last_read =
        current;
}


static void start_if_ready(void)
{
    if (g_started ||
        g_count <
            (uint32_t)(
                (uint64_t)g_rate *
                AUDIO_START_MS /
                1000u)) {
        return;
    }

    /*
     * Oldest queued frame.
     */
    const uint32_t start =
        (g_write_pos +
         AUDIO_RING_FRAMES -
         g_count) %
        AUDIO_RING_FRAMES;

    for (int ch = 0;
         ch < 2;
         ++ch) {

        AXVoiceBegin(
            g_voice[ch]);

        AXSetVoiceCurrentOffset(
            g_voice[ch],
            start);

        reset_voice_src(
            g_voice[ch]);

        AXVoiceEnd(
            g_voice[ch]);

        AXSetVoiceState(
            g_voice[ch],
            AX_VOICE_STATE_PLAYING);
    }

    g_last_read =
        start;

    g_started = 1;
}


static int setup_voice(int channel)
{
    AXVoice *voice =
        AXAcquireVoice(
            31,
            NULL,
            NULL);

    if (!voice) {
        return -1;
    }

    g_voice[channel] =
        voice;

    AXVoiceOffsets offsets;
    AXVoiceVeData volume;

    memset(
        &offsets,
        0,
        sizeof(offsets));

    memset(
        &volume,
        0,
        sizeof(volume));

    volume.volume =
        0x8000;

    offsets.dataType =
        AX_VOICE_FORMAT_LPCM16;

    offsets.loopingEnabled =
        AX_VOICE_LOOP_ENABLED;

    offsets.loopOffset =
        0;

    offsets.endOffset =
        AUDIO_RING_FRAMES - 1u;

    offsets.currentOffset =
        0;

    offsets.data =
        g_pcm[channel];

    AXVoiceBegin(
        voice);

    AXSetVoiceType(
        voice,
        0);

    AXSetVoiceVe(
        voice,
        &volume);

    AXSetVoiceDeviceMix(
        voice,
        AX_DEVICE_TYPE_TV,
        0,
        g_stereo_mix[channel]);

    AXSetVoiceDeviceMix(
        voice,
        AX_DEVICE_TYPE_DRC,
        0,
        g_stereo_mix[channel]);

    AXSetVoiceOffsets(
        voice,
        &offsets);

    reset_voice_src(
        voice);

    AXVoiceEnd(
        voice);

    AXSetVoiceState(
        voice,
        AX_VOICE_STATE_STOPPED);

    return 0;
}


int audio_init(int rate,
               int channels,
               char *why,
               size_t why_size)
{
    if (why &&
        why_size) {
        why[0] = '\0';
    }

    audio_exit();

    if (rate != 48000 ||
        channels != 2) {

        if (why &&
            why_size) {

            snprintf(
                why,
                why_size,
                "AX path expects 48k stereo");
        }

        return -1;
    }

    g_rate =
        rate;

    /*
     * Usually main() already runs on CPU1, the Wii U main core.
     */
    WHBLogPrintf(
        "audio: direct AX init on CPU%u",
        OSGetCoreId());

    if (!AXIsInit()) {
        AXInitParams params;

        memset(
            &params,
            0,
            sizeof(params));

        params.renderer =
            AX_INIT_RENDERER_48KHZ;

        params.pipeline =
            AX_INIT_PIPELINE_SINGLE;

        AXInitWithParams(
            &params);

        g_ax_owned = 1;
    }

    const size_t bytes =
        AUDIO_RING_FRAMES *
        sizeof(int16_t);

    g_pcm[0] =
        ax_alloc(bytes);

    g_pcm[1] =
        ax_alloc(bytes);

    if (!g_pcm[0] ||
        !g_pcm[1]) {

        if (why &&
            why_size) {

            snprintf(
                why,
                why_size,
                "AX PCM allocation failed");
        }

        audio_exit();

        return -1;
    }

    memset(
        g_pcm[0],
        0,
        bytes);

    memset(
        g_pcm[1],
        0,
        bytes);

    DCFlushRange(
        g_pcm[0],
        (uint32_t)bytes);

    DCFlushRange(
        g_pcm[1],
        (uint32_t)bytes);

    if (setup_voice(0) != 0 ||
        setup_voice(1) != 0) {

        if (why &&
            why_size) {

            snprintf(
                why,
                why_size,
                "AX voice acquisition failed");
        }

        audio_exit();

        return -1;
    }

    g_write_pos = 0;
    g_last_read = 0;
    g_count = 0;

    g_started = 0;

    g_packets = 0;
    g_failed = 0;
    g_dropped = 0;
    g_underruns = 0;

    g_input_total = 0;
    g_device_total = 0;
    g_used_total = 0;

    g_diag_at =
        now_ms();

    g_diag_input_at = 0;
    g_diag_device_at = 0;
    g_diag_used_at = 0;

    g_diag_input_fps = 0;
    g_diag_device_fps = 0;
    g_diag_used_fps = 0;

    WHBLogPrintf(
        "audio: direct AX ready input=%uHz frame=%u start=%ums",
        AXGetInputSamplesPerSec(),
        AXGetInputSamplesPerFrame(),
        AUDIO_START_MS);

    return 0;
}


void audio_exit(void)
{
    stop_voices();

    for (int ch = 0;
         ch < 2;
         ++ch) {

        if (g_voice[ch]) {
            AXFreeVoice(
                g_voice[ch]);

            g_voice[ch] =
                NULL;
        }
    }

    free(g_pcm[0]);
    free(g_pcm[1]);

    g_pcm[0] = NULL;
    g_pcm[1] = NULL;

    g_write_pos = 0;
    g_last_read = 0;
    g_count = 0;

    if (g_ax_owned &&
        AXIsInit()) {

        AXQuit();
    }

    g_ax_owned = 0;
}


void audio_push_pcm_s16le(const uint8_t *data,
                          uint32_t size)
{
    if (!g_pcm[0] ||
        !g_pcm[1] ||
        !data ||
        !size) {
        return;
    }

    if ((size & 3u) != 0) {
        g_failed++;
        return;
    }

    const uint32_t frames =
        size / 4u;

    if (!frames ||
        frames >
            AUDIO_MAX_PUSH_FRAMES) {

        g_failed++;
        return;
    }

    audio_sync_position();

    /*
     * For this diagnostic version, a drop means ONLY a genuine full
     * hardware ring. No artificial target trimming exists anymore.
     */
    if (frames >
        AUDIO_RING_FRAMES -
        g_count) {

        g_dropped++;

        return;
    }

    const uint32_t start =
        g_write_pos;

    for (uint32_t i = 0;
         i < frames;
         ++i) {

        const uint32_t dst =
            (start + i) %
            AUDIO_RING_FRAMES;

        const size_t at =
            (size_t)i * 4u;

        const uint16_t left =
            (uint16_t)data[at] |
            ((uint16_t)data[
                at + 1u] << 8);

        const uint16_t right =
            (uint16_t)data[
                at + 2u] |
            ((uint16_t)data[
                at + 3u] << 8);

        /*
         * PPC is big-endian; assigning the numeric sample produces the
         * LPCM16 byte order AX expects.
         */
        g_pcm[0][dst] =
            (int16_t)left;

        g_pcm[1][dst] =
            (int16_t)right;
    }

    /*
     * Flush only the regions just written.
     */
    const uint32_t first =
        frames <
            AUDIO_RING_FRAMES -
            start
        ? frames
        : AUDIO_RING_FRAMES -
            start;

    DCFlushRange(
        &g_pcm[0][start],
        first *
        sizeof(int16_t));

    DCFlushRange(
        &g_pcm[1][start],
        first *
        sizeof(int16_t));

    if (frames > first) {
        const uint32_t second =
            frames -
            first;

        DCFlushRange(
            &g_pcm[0][0],
            second *
            sizeof(int16_t));

        DCFlushRange(
            &g_pcm[1][0],
            second *
            sizeof(int16_t));
    }

    g_write_pos =
        (g_write_pos +
         frames) %
        AUDIO_RING_FRAMES;

    g_count +=
        frames;

    g_packets++;

    g_input_total +=
        frames;

    start_if_ready();
}


void audio_stats(unsigned long *packets,
                 unsigned long *failed,
                 unsigned long *dropped)
{
    audio_sync_position();

    if (packets) {
        *packets =
            g_packets;
    }

    if (failed) {
        *failed =
            g_failed;
    }

    if (dropped) {
        *dropped =
            g_dropped;
    }
}


void audio_diag(AudioDiag *diag)
{
    if (!diag) {
        return;
    }

    memset(
        diag,
        0,
        sizeof(*diag));

    audio_sync_position();

    const uint32_t now =
        now_ms();

    const uint32_t elapsed =
        now -
        g_diag_at;

    if (elapsed >= 1000) {
        g_diag_input_fps =
            (unsigned)(
                (g_input_total -
                 g_diag_input_at) *
                1000ull /
                elapsed);

        g_diag_device_fps =
            (unsigned)(
                (g_device_total -
                 g_diag_device_at) *
                1000ull /
                elapsed);

        g_diag_used_fps =
            (unsigned)(
                (g_used_total -
                 g_diag_used_at) *
                1000ull /
                elapsed);

        g_diag_at =
            now;

        g_diag_input_at =
            g_input_total;

        g_diag_device_at =
            g_device_total;

        g_diag_used_at =
            g_used_total;
    }

    diag->input_fps =
        g_diag_input_fps;

    diag->device_fps =
        g_diag_device_fps;

    diag->used_fps =
        g_diag_used_fps;

    diag->callback_frames =
        AXIsInit()
            ? AXGetInputSamplesPerFrame()
            : 0;

    diag->underruns =
        g_underruns;
}


unsigned audio_queue_ms(void)
{
    audio_sync_position();

    return
        (unsigned)(
            (uint64_t)g_count *
            1000ull /
            (uint64_t)g_rate);
}
