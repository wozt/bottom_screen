#include "stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bs_le.h"

/*
 * The two builds this file serves.
 *
 * On a desktop it is plain POSIX: sockets, pthreads, CLOCK_MONOTONIC,
 * which is what the host-side smoke test compiles. On the console
 * (__WIIU__) wut provides the same BSD sockets, but there is no
 * pthread: the threads and locks become coreinit's OSThread and
 * OSMutex, and the clock becomes OSGetSystemTime. Everything above
 * these few wrappers is shared, because a protocol that drifted between
 * the tested half and the shipped half would be tested nowhere.
 */
#ifdef __WIIU__

#include <coreinit/condition.h>
#include <coreinit/core.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

typedef OSMutex     bs_mtx;
typedef OSCondition bs_cnd;

#define BS_MTX_INIT(m)    OSInitMutex(&(m))
#define BS_MTX_LOCK(m)    OSLockMutex(&(m))
#define BS_MTX_UNLOCK(m)  OSUnlockMutex(&(m))
#define BS_CND_INIT(c)    OSInitCond(&(c))
#define BS_CND_SIGNAL(c)  OSSignalCond(&(c))

/*
 * WUT exposes OSWaitCond(), but no timed condition-variable wait.
 *
 * This helper is only used when the compressed-video queue is full.
 * Temporarily release the queue mutex, sleep for the requested short
 * interval, then reacquire it so the consumer can make progress.
 *
 * The caller checks the queue condition again after this returns.
 */
static int bs_cnd_wait_ms(bs_cnd *c, bs_mtx *m, int ms)
{
    (void)c;

    OSUnlockMutex(m);

    if (ms > 0) {
        OSSleepTicks(
            OSMillisecondsToTicks(
                (uint32_t)ms));
    } else {
        OSYieldThread();
    }

    OSLockMutex(m);

    return 1;
}

static OSThread g_thread;
static uint8_t g_thread_stack[64 * 1024] __attribute__((aligned(0x40)));

static int reader_entry(int argc, const char **argv);

static int bs_thread_start(void)
{
    /* The reader only blocks on the socket; it needs no particular core.
     * Kept off the core the caller runs on, which is where drawing
     * happens. */
    const uint32_t main_core = OSGetCoreId();
    const uint16_t affinity = main_core == 0
        ? OS_THREAD_ATTRIB_AFFINITY_CPU1
        : OS_THREAD_ATTRIB_AFFINITY_CPU0;
    if (!OSCreateThread(&g_thread, reader_entry, 0, NULL,
                        g_thread_stack + sizeof(g_thread_stack),
                        sizeof(g_thread_stack), 16, affinity))
        return -1;
    OSSetThreadName(&g_thread, "bottom-screen RX");
    OSResumeThread(&g_thread);
    return 0;
}

static void bs_thread_join(void)
{
    int result = 0;
    OSJoinThread(&g_thread, &result);
}

static uint32_t bs_now_us(void)
{
    return (uint32_t)OSTicksToMicroseconds(OSGetSystemTime());
}

#else /* host */

#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

typedef pthread_mutex_t bs_mtx;
typedef pthread_cond_t  bs_cnd;

#define BS_MTX_INIT(m)    pthread_mutex_init(&(m), NULL)
#define BS_MTX_LOCK(m)    pthread_mutex_lock(&(m))
#define BS_MTX_UNLOCK(m)  pthread_mutex_unlock(&(m))
#define BS_CND_INIT(c)    pthread_cond_init(&(c), NULL)
#define BS_CND_SIGNAL(c)  pthread_cond_signal(&(c))

static int bs_cnd_wait_ms(bs_cnd *c, bs_mtx *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return pthread_cond_timedwait(c, m, &ts) == 0;
}

static pthread_t g_thread;
static void *reader_entry(void *arg);

static int bs_thread_start(void)
{
    return pthread_create(&g_thread, NULL, reader_entry, NULL);
}

static void bs_thread_join(void)
{
    pthread_join(g_thread, NULL);
}

static uint32_t bs_now_us(void)
{
    /* Truncated to 32 bits to match the protocol's timestamp fields.
     * Wraps every ~71 minutes; only ever used as a relative measure. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000u);
}

#endif /* __WIIU__ */

#include <opus/opus.h>

#define AUDIO_RING_FRAMES 16384   /* about a third of a second */

/*
 * Encoded access units, reader -> decoder worker.
 *
 * Eight slots absorb scheduling and network jitter; they must never
 * become a latency reservoir, so a full queue waits for a bounded time
 * rather than growing. Encoded frames are never dropped silently the
 * way decoded ones can be: a missing AU corrupts every frame that
 * references it until the next keyframe.
 */
#define VIDEO_SLOTS 8

/* How long the reader waits for a free slot before it has to call an AU
 * lost. One decode cycle on the console is single-digit milliseconds;
 * longer than that means the consumer has stopped, not that it is
 * busy. */
#define VIDEO_WAIT_MS 12

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    int      keyframe;
} VideoSlot;

static int        g_sock = -1;
static int        g_stop;
static volatile int g_connected;
static int        g_thread_started;

/* Which screens the server has, as a bit per BsScreen. Bottom only
 * until it says otherwise, which is also what an older server means by
 * saying nothing. */
static volatile int g_screens = 1 << BS_SCREEN_BOTTOM;
static volatile int g_watching[BS_SCREEN_COUNT];

static bs_mtx g_info_lock;
static StreamInfo g_info;

static bs_mtx  g_video_lock;
static bs_cnd  g_video_space;
static VideoSlot g_video[VIDEO_SLOTS];
static unsigned g_video_read, g_video_count;
static uint32_t g_frames;

/* Set when the picture changed shape and the caller has not been told
 * yet. One slot is enough: the queue is flushed when it is set, so the
 * next AU taken after consuming it is the new geometry. */
static int g_resize_pending;
static int g_resize_w, g_resize_h;

/* Sound, as a ring that drops its oldest when it overruns: if the link
 * cannot keep up, the sound worth hearing is the sound from now. */
static bs_mtx  g_audio_lock;
static int16_t *g_audio;
static int      g_audio_head, g_audio_count, g_audio_cap;
static OpusDecoder *g_opus;

/* The question the machine is waiting on, if any; see the Switch
 * client, whose flow this follows exactly. */
static bs_mtx  g_prompt_lock;
static BsPrompt g_prompt;
static char    g_prompt_body[BS_PROMPT_MAX];
static size_t  g_prompt_body_len;
static int     g_prompt_pending;

static char g_error[192];

static void note_error(const char *what)
{
    snprintf(g_error, sizeof(g_error), "%s", what);
}

/* ------------------------------------------------------------ sockets */

static int write_all(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(g_sock, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_exact(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = recv(g_sock, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * One framed message, serialised field by field. Building the header
 * and payload into a single buffer rather than two send() calls keeps
 * the message whole on the wire even though TCP would merge them
 * anyway -- and it is one syscall either way.
 */
static int send_msg(uint8_t type, const void *payload, uint32_t payload_len)
{
    if (g_sock < 0 || !g_connected)
        return -1;
    uint8_t buf[sizeof(BsMsgHeader) + BS_PROMPT_MAX];
    if (payload_len > BS_PROMPT_MAX)
        return -1;
    BsMsgHeader h;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.payload_size = payload_len;
    bs_msg_header_to_le(&h, buf);
    if (payload_len)
        memcpy(buf + sizeof(BsMsgHeader), payload, payload_len);
    return write_all(buf, sizeof(BsMsgHeader) + payload_len);
}

/*
 * Reads one framed message. Returns 0 with *out_len set, -1 when the
 * connection is unusable. The length is an out-parameter so that the
 * protocol's empty messages (PING, PONG) are not confused with the end
 * of the stream -- the bug bs_net.h documents, avoided the same way.
 */
static int recv_msg(uint8_t *type, uint8_t *buf, size_t bufcap, size_t *out_len)
{
    uint8_t hdr[sizeof(BsMsgHeader)];
    if (read_exact(hdr, sizeof(hdr)) != 0)
        return -1;
    BsMsgHeader h;
    bs_msg_header_from_le(&h, hdr);
    if (h.payload_size > BS_MAX_PAYLOAD || h.payload_size > bufcap)
        return -1;
    if (h.payload_size && read_exact(buf, h.payload_size) != 0)
        return -1;
    *type = h.type;
    *out_len = h.payload_size;
    return 0;
}

static int connect_to(const char *host, uint16_t port, char *err, size_t errlen)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
#ifdef __WIIU__
        /* wut's resolver is not something to lean on from here; the
         * console's settings screen takes a dotted address anyway. */
        snprintf(err, errlen, "'%s' is not an IPv4 address", host);
        return -1;
#else
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            snprintf(err, errlen, "cannot resolve %s", host);
            return -1;
        }
        addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
#endif
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "could not create a socket");
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(err, errlen, "cannot reach %s:%u", host, port);
        close(fd);
        return -1;
    }
    return fd;
}

/* -------------------------------------------------------------- video */

/*
 * Pushes one access unit. Waits up to VIDEO_WAIT_MS for a slot when the
 * queue is full; an AU lost to a genuinely stuck consumer is counted
 * and dropped, because the alternative is the reader -- and therefore
 * the whole connection -- blocking behind a decoder that has stopped.
 */
static void push_video(const uint8_t *data, uint32_t size, int keyframe)
{
    BS_MTX_LOCK(g_video_lock);
    if (g_video_count == VIDEO_SLOTS) {
        bs_cnd_wait_ms(&g_video_space, &g_video_lock, VIDEO_WAIT_MS);
        if (g_video_count == VIDEO_SLOTS) {
            BS_MTX_UNLOCK(g_video_lock);
            return;
        }
    }
    VideoSlot *s = &g_video[(g_video_read + g_video_count) % VIDEO_SLOTS];
    if (size > s->capacity) {
        uint8_t *grown = realloc(s->data, size + size / 2 + 4096);
        if (!grown) {
            BS_MTX_UNLOCK(g_video_lock);
            return;
        }
        s->data = grown;
        s->capacity = size + size / 2 + 4096;
    }
    memcpy(s->data, data, size);
    s->size = size;
    s->keyframe = keyframe;
    g_video_count++;
    g_frames++;
    BS_MTX_UNLOCK(g_video_lock);
}

int stream_take_video(uint8_t *out, uint32_t cap, uint32_t *out_size,
                      int *keyframe)
{
    int got = 0;
    BS_MTX_LOCK(g_video_lock);
    if (g_video_count) {
        VideoSlot *s = &g_video[g_video_read];
        if (s->size <= cap) {
            memcpy(out, s->data, s->size);
            *out_size = s->size;
            *keyframe = s->keyframe;
            got = 1;
        }
        /* Too small a caller buffer loses the AU rather than truncating
         * it: half an access unit decodes to garbage and poisons the
         * reference chain just the same. */
        g_video_read = (g_video_read + 1) % VIDEO_SLOTS;
        g_video_count--;
        BS_CND_SIGNAL(g_video_space);
    }
    BS_MTX_UNLOCK(g_video_lock);
    return got;
}

int stream_take_resize(int *w, int *h)
{
    int got = 0;
    BS_MTX_LOCK(g_video_lock);
    if (g_resize_pending) {
        *w = g_resize_w;
        *h = g_resize_h;
        g_resize_pending = 0;
        got = 1;
    }
    BS_MTX_UNLOCK(g_video_lock);
    return got;
}

/* -------------------------------------------------------------- sound */

static void store_audio(const int16_t *pcm, int frames, int channels)
{
    BS_MTX_LOCK(g_audio_lock);
    for (int i = 0; i < frames; i++) {
        if (g_audio_count == g_audio_cap) {
            g_audio_head = (g_audio_head + 1) % g_audio_cap;
            g_audio_count--;
        }
        int slot = (g_audio_head + g_audio_count) % g_audio_cap;
        g_audio[slot * 2 + 0] = pcm[i * channels + 0];
        g_audio[slot * 2 + 1] = pcm[i * channels + (channels > 1 ? 1 : 0)];
        g_audio_count++;
    }
    BS_MTX_UNLOCK(g_audio_lock);
}

int stream_take_audio(int16_t *out, int max_frames)
{
    BS_MTX_LOCK(g_audio_lock);
    int n = g_audio_count < max_frames ? g_audio_count : max_frames;
    for (int i = 0; i < n; i++) {
        int slot = (g_audio_head + i) % g_audio_cap;
        out[i * 2 + 0] = g_audio[slot * 2 + 0];
        out[i * 2 + 1] = g_audio[slot * 2 + 1];
    }
    g_audio_head = (g_audio_head + n) % g_audio_cap;
    g_audio_count -= n;
    BS_MTX_UNLOCK(g_audio_lock);
    return n;
}

/* ------------------------------------------------------------- reader */

static void reader_run(void)
{
    uint8_t *buf = malloc(BS_MAX_PAYLOAD);
    int16_t *pcm = malloc(sizeof(int16_t) * 5760 * 2);  /* 120 ms at 48 kHz */
    if (!buf || !pcm) {
        note_error("out of memory");
        g_connected = 0;
        free(buf);
        free(pcm);
        return;
    }

    while (!g_stop) {
        uint8_t type = 0;
        size_t n = 0;
        if (recv_msg(&type, buf, BS_MAX_PAYLOAD, &n) != 0) {
            note_error("the connection ended");
            break;
        }

        if (type == BS_MSG_VIDEO && n > sizeof(BsVideoHeader)) {
            /*
             * Not decoded here. H.264 frames are not independent, so the
             * encoded AU goes to the decoder's queue whole; dropping one
             * would break every frame after it until the next keyframe.
             * The header is still read -- from the bytes, never memcpy'd,
             * this CPU being big-endian -- for its keyframe flag.
             */
            BsVideoHeader vh;
            bs_video_header_from_le(&vh, buf);
            push_video(buf + sizeof(BsVideoHeader),
                       (uint32_t)(n - sizeof(BsVideoHeader)),
                       (vh.flags & BS_VFLAG_KEYFRAME) != 0);
        } else if (type == BS_MSG_AUDIO && n > sizeof(BsAudioHeader) && g_opus) {
            int frames = opus_decode(g_opus, buf + sizeof(BsAudioHeader),
                                     (opus_int32)(n - sizeof(BsAudioHeader)),
                                     pcm, 5760, 0);
            if (frames > 0)
                store_audio(pcm, frames, 2);
        } else if (type == BS_MSG_STREAM_INFO && n >= sizeof(BsStreamInfo)) {
            BsStreamInfo si;
            bs_stream_info_from_le(&si, buf);
            /*
             * The emulator's internal resolution moved. The connection
             * survives it: the server renegotiates rather than dropping
             * us, so the picture simply changes size.
             *
             * Frames still queued are the old geometry, and the decoder
             * is rebuilt by the caller the moment it takes the resize --
             * so the queue is emptied here rather than handed AUs of a
             * shape its decoder no longer matches. The server sends a
             * keyframe after the change, which is why there is no
             * REQUEST_KEYFRAME from this client: there is one encoder
             * behind every client and a request is billed to all of
             * them. from_frame_id is ignored, the same choice the
             * Switch makes: with the queue flushed there is nothing old
             * left to confuse with the new.
             */
            BS_MTX_LOCK(g_info_lock);
            const int changed = (si.width != g_info.width ||
                                 si.height != g_info.height);
            g_info.width = si.width;
            g_info.height = si.height;
            if (si.fps > 0) g_info.fps = si.fps;
            BS_MTX_UNLOCK(g_info_lock);

            if (changed) {
                BS_MTX_LOCK(g_video_lock);
                g_video_read = 0;
                g_video_count = 0;
                g_resize_pending = 1;
                g_resize_w = si.width;
                g_resize_h = si.height;
                BS_CND_SIGNAL(g_video_space);
                BS_MTX_UNLOCK(g_video_lock);
            }
        } else if (type == BS_MSG_SCREENS && n >= sizeof(BsScreens)) {
            /* Which screens this server has, and how many are watching
             * each. A server built before the top screen existed sends
             * nothing, so the mask stays "bottom only". */
            BsScreens sc;
            bs_screens_from_le(&sc, buf);
            g_screens = sc.available;
            g_watching[BS_SCREEN_BOTTOM] = sc.watching_bottom;
            g_watching[BS_SCREEN_TOP] = sc.watching_top;
        } else if (type == BS_MSG_PROMPT && n >= sizeof(BsPrompt)) {
            BS_MTX_LOCK(g_prompt_lock);
            bs_prompt_from_le(&g_prompt, buf);
            g_prompt_body_len = n - sizeof(BsPrompt);
            if (g_prompt_body_len > sizeof(g_prompt_body))
                g_prompt_body_len = sizeof(g_prompt_body);
            memcpy(g_prompt_body, buf + sizeof(BsPrompt), g_prompt_body_len);
            /* id 0 withdraws it: the game stopped waiting, so whatever
             * is on screen for it should go. */
            g_prompt_pending = (g_prompt.id != 0);
            BS_MTX_UNLOCK(g_prompt_lock);
        } else if (type == BS_MSG_PING) {
            send_msg(BS_MSG_PONG, NULL, 0);
        }
    }

    free(buf);
    free(pcm);
    g_connected = 0;
}

#ifdef __WIIU__
static int reader_entry(int argc, const char **argv)
{
    (void)argc;
    (void)argv;
    reader_run();
    return 0;
}
#else
static void *reader_entry(void *arg)
{
    (void)arg;
    reader_run();
    return NULL;
}
#endif

/* --------------------------------------------------------- lifecycle */

static void locks_init_once(void)
{
    static int done;
    if (done)
        return;
    BS_MTX_INIT(g_info_lock);
    BS_MTX_INIT(g_video_lock);
    BS_CND_INIT(g_video_space);
    BS_MTX_INIT(g_audio_lock);
    BS_MTX_INIT(g_prompt_lock);
    done = 1;
}

int stream_connect(const char *host, uint16_t port, char *err, size_t errlen)
{
    locks_init_once();
    stream_disconnect();
    g_error[0] = '\0';
    g_stop = 0;

    g_sock = connect_to(host, port, err, errlen);
    if (g_sock < 0)
        return -1;

    /* The hello, and every byte after it, serialised field by field:
     * this CPU's natural byte order is the wire's reverse. */
    BsHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = BS_MAGIC;
    hello.version = BS_VERSION;
    uint8_t wire[sizeof(BsHelloAck)];
    bs_hello_to_le(&hello, wire);
    if (write_all(wire, sizeof(BsHello)) != 0) {
        snprintf(err, errlen, "could not say hello");
        goto fail;
    }

    if (read_exact(wire, sizeof(BsHelloAck)) != 0) {
        snprintf(err, errlen, "no usable answer from the server");
        goto fail;
    }
    BsHelloAck ack;
    bs_hello_ack_from_le(&ack, wire);
    if (ack.magic != BS_MAGIC) {
        snprintf(err, errlen, "no usable answer from the server");
        goto fail;
    }
    if (!ack.accepted) {
        snprintf(err, errlen, "the server is full");
        goto fail;
    }

    /* The SPS/PPS, queued as the very first access unit so the decoder
     * sees the parameter sets before any slice. */
    uint8_t extradata[4096];
    if (ack.extradata_size > sizeof(extradata)) {
        snprintf(err, errlen, "extradata too large");
        goto fail;
    }
    if (ack.extradata_size &&
        read_exact(extradata, ack.extradata_size) != 0) {
        snprintf(err, errlen, "truncated extradata");
        goto fail;
    }

    BS_MTX_LOCK(g_info_lock);
    g_info.width = ack.width;
    g_info.height = ack.height;
    g_info.console = ack.console;
    g_info.fps = ack.fps > 0 ? ack.fps : 60;
    g_info.audio_rate = ack.audio_rate;
    g_info.audio_channels = ack.audio_channels;
    BS_MTX_UNLOCK(g_info_lock);

    if (ack.audio_rate > 0) {
        int oerr = 0;
        g_opus = opus_decoder_create(48000, 2, &oerr);
        if (oerr != OPUS_OK)
            g_opus = NULL;      /* sound is optional; the picture is not */
        if (!g_audio) {
            g_audio_cap = AUDIO_RING_FRAMES;
            g_audio = malloc(sizeof(int16_t) * 2 * (size_t)g_audio_cap);
        }
        g_audio_head = g_audio_count = 0;
    }

    g_frames = 0;
    g_connected = 1;

    if (ack.extradata_size)
        push_video(extradata, ack.extradata_size, 1);

    if (bs_thread_start() != 0) {
        snprintf(err, errlen, "could not start the reader");
        g_connected = 0;
        goto fail;
    }
    g_thread_started = 1;

    /* Nothing decodes until a keyframe. It waits for the stream's own
     * rather than asking for one: there is a single encoder behind all
     * the clients, so a request is billed to every one of them. */
    return 0;

fail:
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    return -1;
}

void stream_disconnect(void)
{
    g_stop = 1;
    /* Breaks the reader out of its blocking read; closing the socket
     * from under it would be a use-after-free race instead. */
    if (g_sock >= 0)
        shutdown(g_sock, SHUT_RDWR);
    if (g_thread_started) {
        bs_thread_join();
        g_thread_started = 0;
    }
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    if (g_opus) {
        opus_decoder_destroy(g_opus);
        g_opus = NULL;
    }
    g_connected = 0;

    BS_MTX_LOCK(g_video_lock);
    g_video_read = 0;
    g_video_count = 0;
    g_resize_pending = 0;
    BS_MTX_UNLOCK(g_video_lock);
}

int stream_connected(void) { return g_connected; }
uint32_t stream_frames(void) { return g_frames; }
const char *stream_last_error(void) { return g_error; }

void stream_info(StreamInfo *out)
{
    BS_MTX_LOCK(g_info_lock);
    *out = g_info;
    BS_MTX_UNLOCK(g_info_lock);
}

/* --------------------------------------------------------------- input */

static uint32_t g_sequence;

static void send_event(uint8_t type, uint8_t code, int16_t x, int16_t y)
{
    if (!g_connected)
        return;
    BsInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.sequence = g_sequence++;
    ev.timestamp_us = bs_now_us();
    ev.type = type;
    ev.code = code;
    ev.x = x;
    ev.y = y;
    uint8_t wire[sizeof(BsInputEvent)];
    bs_input_event_to_le(&ev, wire);
    send_msg(BS_MSG_INPUT, wire, sizeof(wire));
}

void stream_send_touch(int type, int x, int y)
{
    send_event((uint8_t)type, 0, (int16_t)x, (int16_t)y);
}

void stream_send_button(int code, int pressed)
{
    send_event(pressed ? BS_INPUT_BUTTON_DOWN : BS_INPUT_BUTTON_UP,
               (uint8_t)code, 0, 0);
}

void stream_send_axis(int code, int value)
{
    send_event(BS_INPUT_AXIS, (uint8_t)code, (int16_t)value, 0);
}

/* ----------------------------------------------------- the other sends */

/*
 * Asks for the machine's other screen. There is no acknowledgement to
 * wait for: the other picture is a different size, and that arrives as
 * a stream info message like any other change of shape.
 */
void stream_send_screen(int screen)
{
    if (!g_connected)
        return;
    BsScreenChoice ch;
    memset(&ch, 0, sizeof(ch));
    ch.screen = (uint8_t)screen;
    uint8_t wire[sizeof(BsScreenChoice)];
    bs_screen_choice_to_le(&ch, wire);
    send_msg(BS_MSG_SET_SCREEN, wire, sizeof(wire));
}

int stream_screens(void)
{
    return g_screens;
}

int stream_watching(int screen)
{
    if (screen < 0 || screen >= BS_SCREEN_COUNT)
        return 0;
    return g_watching[screen];
}

void stream_send_size(int width, int height)
{
    if (!g_connected)
        return;
    BsSize sz = { (uint16_t)width, (uint16_t)height };
    uint8_t wire[sizeof(BsSize)];
    bs_size_to_le(&sz, wire);
    send_msg(BS_MSG_SET_SIZE, wire, sizeof(wire));
}

void stream_send_quality(int bitrate)
{
    if (!g_connected)
        return;
    BsQuality q;
    memset(&q, 0, sizeof(q));
    q.bitrate = (uint32_t)bitrate;
    uint8_t wire[sizeof(BsQuality)];
    bs_quality_to_le(&q, wire);
    send_msg(BS_MSG_SET_QUALITY, wire, sizeof(wire));
}

/* A Wii U mixes for the television and for the GamePad's own speakers
 * at once, and they do not carry the same thing. Meaningless on the
 * other two consoles. */
void stream_send_audio_source(int source)
{
    if (!g_connected)
        return;
    BsAudioChoice ac;
    memset(&ac, 0, sizeof(ac));
    ac.source = (uint8_t)source;
    uint8_t wire[sizeof(BsAudioChoice)];
    bs_audio_choice_to_le(&ac, wire);
    send_msg(BS_MSG_SET_AUDIO_SOURCE, wire, sizeof(wire));
}

uint16_t stream_take_prompt(BsPrompt *out, char *body, size_t bodylen)
{
    if (!g_prompt_pending)
        return 0;
    uint16_t id = 0;
    BS_MTX_LOCK(g_prompt_lock);
    if (g_prompt_pending) {
        g_prompt_pending = 0;
        *out = g_prompt;
        size_t n = g_prompt_body_len;
        if (n >= bodylen) n = bodylen - 1;
        memcpy(body, g_prompt_body, n);
        body[n] = '\0';
        id = g_prompt.id;
    }
    BS_MTX_UNLOCK(g_prompt_lock);
    return id;
}

void stream_send_prompt_reply(uint16_t id, int cancelled, int choice,
                              const char *text)
{
    if (!g_connected)
        return;
    /* The reply's fixed part and its text travel as one payload; the
     * text simply follows the four bytes, so they share a buffer. */
    uint8_t wire[sizeof(BsPromptReply) + BS_PROMPT_MAX];
    BsPromptReply rp;
    memset(&rp, 0, sizeof(rp));
    rp.id = id;
    rp.cancelled = (uint8_t)(cancelled ? 1 : 0);
    rp.choice = (uint8_t)choice;
    bs_prompt_reply_to_le(&rp, wire);
    size_t n = text ? strlen(text) : 0;
    if (n > BS_PROMPT_MAX) n = BS_PROMPT_MAX;
    if (n)
        memcpy(wire + sizeof(BsPromptReply), text, n);
    send_msg(BS_MSG_PROMPT_REPLY, wire,
             (uint32_t)(sizeof(BsPromptReply) + n));
}
