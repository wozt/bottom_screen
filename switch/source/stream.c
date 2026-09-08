#include "stream.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>

#include <opus/opus.h>

#include "bs_decoder.h"
#include "bs_net.h"

#define AUDIO_RING_FRAMES 16384      /* about a third of a second */

static BsConn    *g_conn;
static BsDecoder *g_dec;
static pthread_t  g_reader;
static int        g_reader_started;
static volatile int g_stop;
static volatile int g_connected;
/* Which screens the server has, as a bit per BsScreen. Bottom only
 * until it says otherwise, which is also what an older server means by
 * saying nothing. */
static volatile int g_screens = 1 << BS_SCREEN_BOTTOM;

static StreamInfo g_info;
static pthread_mutex_t g_info_lock = PTHREAD_MUTEX_INITIALIZER;

/* The newest decoded picture, and whether anybody has taken it yet. */
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_y, *g_u, *g_v;
static int      g_fw, g_fh;
static int      g_frame_pending;
static uint32_t g_frames;

/* Sound, as a ring that drops its oldest when it overruns: if the link
 * cannot keep up, the sound worth hearing is the sound from now. */
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t *g_audio;
static int      g_audio_head, g_audio_count, g_audio_cap;
static OpusDecoder *g_opus;

static char g_error[192];

static void note_error(const char *what)
{
    snprintf(g_error, sizeof(g_error), "%s", what);
}

/* ------------------------------------------------------------- frames */

static void store_frame(const BsDecodedFrame *f)
{
    pthread_mutex_lock(&g_frame_lock);

    if (f->width != g_fw || f->height != g_fh) {
        free(g_y); free(g_u); free(g_v);
        g_fw = f->width;
        g_fh = f->height;
        g_y = malloc((size_t)g_fw * g_fh);
        g_u = malloc((size_t)(g_fw / 2) * (g_fh / 2));
        g_v = malloc((size_t)(g_fw / 2) * (g_fh / 2));
    }
    if (!g_y || !g_u || !g_v) {
        pthread_mutex_unlock(&g_frame_lock);
        return;
    }

    /* Packed on the way in, so the main thread can upload one run per
     * plane instead of walking strides it would have to be told about. */
    for (int r = 0; r < g_fh; r++)
        memcpy(g_y + (size_t)r * g_fw, f->y + (size_t)r * f->y_stride, g_fw);
    for (int r = 0; r < g_fh / 2; r++) {
        memcpy(g_u + (size_t)r * (g_fw / 2), f->u + (size_t)r * f->u_stride, g_fw / 2);
        memcpy(g_v + (size_t)r * (g_fw / 2), f->v + (size_t)r * f->v_stride, g_fw / 2);
    }

    g_frame_pending = 1;
    g_frames++;
    pthread_mutex_unlock(&g_frame_lock);
}

/*
 * The size of the picture actually decoded, which is not always the size
 * the server last announced.
 *
 * The two disagree for a frame or two around any change of shape --
 * frames already in flight carry the old one -- and they disagree for
 * good if a decoder refuses to follow. Drawing from the announcement
 * meant that a disagreement showed as a frozen picture rather than as
 * anything anybody could diagnose. Returns 0 before the first frame.
 */
int stream_picture_size(int *w, int *h)
{
    pthread_mutex_lock(&g_frame_lock);
    const int have = g_fw > 0 && g_fh > 0;
    if (have) { *w = g_fw; *h = g_fh; }
    pthread_mutex_unlock(&g_frame_lock);
    return have;
}

int stream_take_frame(uint8_t *y, uint8_t *u, uint8_t *v,
                      int y_stride, int uv_stride, int width, int height)
{
    int got = 0;
    pthread_mutex_lock(&g_frame_lock);
    if (g_frame_pending && g_y && width == g_fw && height == g_fh) {
        for (int r = 0; r < height; r++)
            memcpy(y + (size_t)r * y_stride, g_y + (size_t)r * g_fw, width);
        for (int r = 0; r < height / 2; r++) {
            memcpy(u + (size_t)r * uv_stride, g_u + (size_t)r * (width / 2), width / 2);
            memcpy(v + (size_t)r * uv_stride, g_v + (size_t)r * (width / 2), width / 2);
        }
        g_frame_pending = 0;
        got = 1;
    }
    pthread_mutex_unlock(&g_frame_lock);
    return got;
}

/* -------------------------------------------------------------- sound */

static void store_audio(const int16_t *pcm, int frames, int channels)
{
    pthread_mutex_lock(&g_audio_lock);
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
    pthread_mutex_unlock(&g_audio_lock);
}

int stream_take_audio(int16_t *out, int max_frames)
{
    pthread_mutex_lock(&g_audio_lock);
    int n = g_audio_count < max_frames ? g_audio_count : max_frames;
    for (int i = 0; i < n; i++) {
        int slot = (g_audio_head + i) % g_audio_cap;
        out[i * 2 + 0] = g_audio[slot * 2 + 0];
        out[i * 2 + 1] = g_audio[slot * 2 + 1];
    }
    g_audio_head = (g_audio_head + n) % g_audio_cap;
    g_audio_count -= n;
    pthread_mutex_unlock(&g_audio_lock);
    return n;
}

/* ------------------------------------------------------------- reader */

static void *reader(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(BS_MAX_PAYLOAD);
    int16_t *pcm = malloc(sizeof(int16_t) * 5760 * 2);   /* 120 ms at 48 kHz */
    if (!buf || !pcm) {
        note_error("out of memory");
        g_connected = 0;
        free(buf); free(pcm);
        return NULL;
    }

    while (!g_stop) {
        uint8_t type = 0;
        size_t n = 0;
        if (bs_recv_msg(g_conn, &type, buf, BS_MAX_PAYLOAD, &n) != 0) {
            note_error("the connection ended");
            break;
        }

        if (type == BS_MSG_VIDEO && n > sizeof(BsVideoHeader)) {
            BsDecodedFrame f;
            int rc = bs_decoder_decode(g_dec, buf + sizeof(BsVideoHeader),
                                       n - sizeof(BsVideoHeader), &f);
            if (rc == 1)
                store_frame(&f);
        } else if (type == BS_MSG_AUDIO && n > sizeof(BsAudioHeader) && g_opus) {
            int frames = opus_decode(g_opus, buf + sizeof(BsAudioHeader),
                                     (opus_int32)(n - sizeof(BsAudioHeader)),
                                     pcm, 5760, 0);
            if (frames > 0)
                store_audio(pcm, frames, 2);
        } else if (type == BS_MSG_STREAM_INFO && n >= sizeof(BsStreamInfo)) {
            BsStreamInfo si;
            memcpy(&si, buf, sizeof(si));
            /*
             * The emulator's internal resolution moved. The connection
             * survives it: the server renegotiates rather than dropping
             * us, so the picture simply changes size.
             */
            pthread_mutex_lock(&g_info_lock);
            const int changed = (si.width != g_info.width ||
                                 si.height != g_info.height);
            g_info.width = si.width;
            g_info.height = si.height;
            if (si.fps > 0) g_info.fps = si.fps;
            pthread_mutex_unlock(&g_info_lock);

            /*
             * A new size means a new decoder.
             *
             * This client was the only one of the three that did not do
             * this, and it got away with it for as long as the only
             * thing that changed the size was somebody moving an
             * emulator's internal resolution -- rare, and forgiving on
             * a software decoder, which reconfigures itself. The console
             * does not use one: h264_nvtegra is the hardware block, set
             * up from the first stream it was given, and it does not
             * reconfigure. Switching screens made that a thing people
             * do on purpose, and the result was a picture that simply
             * stopped changing -- the request left, the server switched,
             * the frames arrived, and nothing decoded to a size the
             * drawing side would accept, so the last good picture stayed
             * on screen looking exactly like a setting that does
             * nothing.
             *
             * Safe here: this thread is the only one that decodes.
             * Nothing is needed for the parameter sets either, because
             * the encoder puts SPS/PPS in front of every keyframe and
             * the server holds everything back until the next one.
             */
            if (changed && g_dec) {
                bs_decoder_destroy(g_dec);
                char derr[128] = "";
                g_dec = bs_decoder_create(derr, sizeof(derr));
                pthread_mutex_lock(&g_frame_lock);
                g_frame_pending = 0;
                pthread_mutex_unlock(&g_frame_lock);
            }
        } else if (type == BS_MSG_SCREENS && n >= sizeof(BsScreens)) {
            /*
             * Which screens this server has. A bit per BsScreen, and a
             * server built before the top screen existed sends nothing
             * -- so the mask stays at "bottom only" and the menu never
             * offers a choice that would do nothing.
             */
            BsScreens sc;
            memcpy(&sc, buf, sizeof(sc));
            g_screens = sc.available;
        } else if (type == BS_MSG_PING) {
            bs_send_msg(g_conn, BS_MSG_PONG, NULL, 0, NULL, 0);
        }
    }

    free(buf);
    free(pcm);
    g_connected = 0;
    return NULL;
}

/* --------------------------------------------------------- lifecycle */

int stream_connect(const char *host, uint16_t port, char *err, size_t errlen)
{
    stream_disconnect();
    g_error[0] = '\0';
    g_stop = 0;

    g_conn = bs_connect(host, port, err, errlen);
    if (!g_conn)
        return -1;

    BsHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = BS_MAGIC;
    hello.version = BS_VERSION;
    if (bs_write_all(g_conn, &hello, sizeof(hello)) != 0) {
        snprintf(err, errlen, "could not say hello");
        goto fail;
    }

    BsHelloAck ack;
    if (bs_read_exact(g_conn, &ack, sizeof(ack)) != 0 || ack.magic != BS_MAGIC) {
        snprintf(err, errlen, "no usable answer from the server");
        goto fail;
    }
    if (!ack.accepted) {
        snprintf(err, errlen, "the server is full");
        goto fail;
    }

    /* The decoder wants the SPS/PPS before the first frame, and the
     * server sends them here rather than only in the stream. */
    uint8_t extradata[4096];
    if (ack.extradata_size > sizeof(extradata)) {
        snprintf(err, errlen, "extradata too large");
        goto fail;
    }
    if (ack.extradata_size &&
        bs_read_exact(g_conn, extradata, ack.extradata_size) != 0) {
        snprintf(err, errlen, "truncated extradata");
        goto fail;
    }

    pthread_mutex_lock(&g_info_lock);
    g_info.width = ack.width;
    g_info.height = ack.height;
    g_info.console = ack.console;
    g_info.fps = ack.fps > 0 ? ack.fps : 60;
    g_info.audio_rate = ack.audio_rate;
    g_info.audio_channels = ack.audio_channels;
    pthread_mutex_unlock(&g_info_lock);

    g_dec = bs_decoder_create(err, errlen);
    if (!g_dec)
        goto fail;

    if (ack.extradata_size) {
        BsDecodedFrame f;
        bs_decoder_decode(g_dec, extradata, ack.extradata_size, &f);
    }

    if (ack.audio_rate > 0) {
        int oerr = 0;
        g_opus = opus_decoder_create(48000, 2, &oerr);
        if (oerr != OPUS_OK)
            g_opus = NULL;      /* sound is optional; the picture is not */
        if (!g_audio) {
            g_audio_cap = AUDIO_RING_FRAMES;
            g_audio = malloc(sizeof(int16_t) * 2 * g_audio_cap);
        }
        g_audio_head = g_audio_count = 0;
    }

    g_frames = 0;
    g_connected = 1;
    if (pthread_create(&g_reader, NULL, reader, NULL) != 0) {
        snprintf(err, errlen, "could not start the reader");
        g_connected = 0;
        goto fail;
    }
    g_reader_started = 1;

    /* Nothing decodes until a keyframe. It waits for the stream's own
     * rather than asking for one: there is a single encoder behind all
     * the clients, so a request is billed to every one of them. */
    return 0;

fail:
    if (g_dec) { bs_decoder_destroy(g_dec); g_dec = NULL; }
    if (g_conn) { bs_conn_close(g_conn); g_conn = NULL; }
    return -1;
}

void stream_disconnect(void)
{
    g_stop = 1;
    /* Breaks the reader out of its blocking read; closing the socket
     * from under it would be a use-after-free race instead. */
    if (g_conn)
        shutdown(bs_conn_fd(g_conn), SHUT_RDWR);
    if (g_reader_started) {
        pthread_join(g_reader, NULL);
        g_reader_started = 0;
    }
    if (g_dec) { bs_decoder_destroy(g_dec); g_dec = NULL; }
    if (g_conn) { bs_conn_close(g_conn); g_conn = NULL; }
    if (g_opus) { opus_decoder_destroy(g_opus); g_opus = NULL; }
    g_connected = 0;
    g_frame_pending = 0;
}

int stream_connected(void) { return g_connected; }
uint32_t stream_frames(void) { return g_frames; }
const char *stream_last_error(void) { return g_error; }

const char *stream_decoder_name(void)
{
    return g_dec ? bs_decoder_name(g_dec) : "none";
}

void stream_info(StreamInfo *out)
{
    pthread_mutex_lock(&g_info_lock);
    *out = g_info;
    pthread_mutex_unlock(&g_info_lock);
}

/* --------------------------------------------------------------- input */

static uint32_t g_sequence;

static void send_event(uint8_t type, uint8_t code, int16_t x, int16_t y)
{
    if (!g_conn || !g_connected)
        return;
    BsInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.sequence = g_sequence++;
    ev.timestamp_us = bs_now_us();
    ev.type = type;
    ev.code = code;
    ev.x = x;
    ev.y = y;
    bs_send_msg(g_conn, BS_MSG_INPUT, &ev, sizeof(ev), NULL, 0);
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

/*
 * Asks for the machine's other screen: the top one on a DS or a 3DS, the
 * television picture on a Wii U.
 *
 * There is no acknowledgement to wait for. The other screen is a
 * different size, and that arrives as a stream info message like any
 * other change of shape -- which is the same path the picture already
 * takes when somebody moves an emulator's internal resolution.
 */
void stream_send_screen(int screen)
{
    if (!g_conn || !g_connected)
        return;
    BsScreenChoice ch;
    memset(&ch, 0, sizeof(ch));
    ch.screen = (uint8_t)screen;
    bs_send_msg(g_conn, BS_MSG_SET_SCREEN, &ch, sizeof(ch), NULL, 0);
}

/* A bit per BsScreen. Bottom only until a server says otherwise. */
int stream_screens(void)
{
    return g_screens;
}

void stream_send_size(int width, int height)
{
    if (!g_conn || !g_connected)
        return;
    BsSize sz = { (uint16_t)width, (uint16_t)height };
    bs_send_msg(g_conn, BS_MSG_SET_SIZE, &sz, sizeof(sz), NULL, 0);
}

/*
 * A Wii U mixes for the television and for the GamePad's own speakers at
 * once, and they do not carry the same thing. Meaningless on the other
 * two consoles, which is why the menu only offers it here.
 */
void stream_send_audio_source(int source)
{
    if (!g_conn)
        return;
    BsAudioChoice ac;
    memset(&ac, 0, sizeof(ac));
    ac.source = (uint8_t)source;
    bs_send_msg(g_conn, BS_MSG_SET_AUDIO_SOURCE, &ac, sizeof(ac), NULL, 0);
}

/* The bitrate to re-encode at; zero lets the server derive one. Shared
 * between clients, like the size. */
void stream_send_quality(int bitrate)
{
    if (!g_conn)
        return;
    BsQuality q;
    memset(&q, 0, sizeof(q));
    q.bitrate = (uint32_t)bitrate;
    bs_send_msg(g_conn, BS_MSG_SET_QUALITY, &q, sizeof(q), NULL, 0);
}

