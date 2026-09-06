#include "bs_server.h"
#include "bs_audio.h"
#include "bs_encoder.h"
#include "bs_net.h"
#include "bs_protocol.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Several clients at once, off one encoder.
 *
 * The picture is encoded once and the same packets go to everyone. That
 * is the whole reason this is not simply the old one-client server run
 * N times: encoding is the expensive part, and a second viewer should
 * cost bandwidth, not another core.
 *
 * Each client gets its own sending thread and its own queue, because the
 * one thing a shared encoder must not do is let the slowest client set
 * the pace. A phone on bad wifi fills its own queue and, past a point,
 * is resynchronised; the others never notice.
 */

#define BS_CLIENTS_DEFAULT 4

/*
 * How much a client may fall behind before it is resynchronised rather
 * than waited for. At the bitrates in play this is a second or so of
 * video -- long enough to ride out a hiccup, short enough that nobody
 * watches a queue drain in slow motion.
 */
#define BS_QUEUE_MAX_BYTES (512u * 1024u)

typedef struct BsPacket {
    struct BsPacket *next;
    uint8_t  type;
    uint8_t  keyframe;
    uint8_t  head_len;
    uint8_t  head[16];
    size_t   len;
    uint8_t  data[];
} BsPacket;

typedef struct {
    BsServer *srv;
    BsConn   *conn;

    pthread_t recv_tid, send_tid;
    int       recv_started, send_started;

    volatile int in_use;   /* the slot is taken */
    volatile int ready;    /* the handshake is done; packets may flow */
    volatile int gone;     /* the client left, or we gave up on it */

    pthread_mutex_t lock;
    pthread_cond_t  cond;
    BsPacket *head, *tail;
    size_t    queued;

    /*
     * Set after a queue overflow: everything buffered is thrown away and
     * nothing is sent until the next keyframe.
     *
     * Encoded video cannot be dropped the way raw frames can. A raw
     * frame stands alone, so latest-wins is right; a P frame is a
     * correction to the one before it, and skipping one leaves the
     * decoder producing garbage until the next keyframe anyway. So we
     * skip deliberately, all the way to that keyframe, instead of
     * showing the mess in between.
     */
    int       want_key;

    uint32_t  frames_sent;
    uint32_t  audio_seq;
    uint32_t  buttons;     /* what this client is holding down */
} BsClient;

struct BsServer {
    BsSource      *source;
    BsSourceInfo   info;
    BsEncoder     *enc;
    BsAudioEncoder *aenc;      /* NULL when the source is silent */
    BsServerConfig cfg;

    int       listen_fd;
    uint16_t  port;          /* the one actually bound, not the one asked for */

    pthread_t accept_thread;
    int       accept_started;
    pthread_t pump_thread;
    int       pump_started;

    volatile int stop;
    volatile uint32_t frames;

    /*
     * A quality change arrives on a client's thread but is applied by
     * the video loop, between two frames. Swapping the encoder from
     * under a bs_encoder_encode already in progress would be a
     * use-after-free with a very confusing crash.
     *
     * With several clients the encoder is shared, so the setting is too:
     * the last person to move it wins. Giving each client its own
     * bitrate would mean an encoder each, which is the cost this whole
     * design exists to avoid.
     */
    volatile int pending_bitrate;
    volatile int quality_dirty;

    BsClient *clients;
    int       max_clients;

    /* Guards the roster: in_use, gone, and the merged button state. */
    pthread_mutex_t roster;
    pthread_cond_t  roster_cond;   /* wakes the pump when someone arrives */

    uint32_t  merged_buttons;
};

/* ---------------------------------------------------------------- queue */

static void packet_free_chain(BsPacket *p)
{
    while (p) {
        BsPacket *next = p->next;
        free(p);
        p = next;
    }
}

/* Called with cl->lock held. */
static void client_purge(BsClient *cl)
{
    packet_free_chain(cl->head);
    cl->head = cl->tail = NULL;
    cl->queued = 0;
}

/*
 * Hands one message to a client's sending thread. Never blocks on the
 * network, so the caller -- the encoder callback, on the pump thread --
 * runs at the speed of the emulator rather than the speed of the worst
 * connection.
 */
static void client_send(BsClient *cl, uint8_t type, int keyframe,
                        const void *head, size_t head_len,
                        const void *body, size_t body_len)
{
    if (!cl->ready || cl->gone)
        return;

    pthread_mutex_lock(&cl->lock);

    if (cl->want_key) {
        /* Waiting to resynchronise. Audio is self-contained, so it can
         * carry on; video waits for the keyframe that makes it mean
         * something again. */
        if (type == BS_MSG_VIDEO && !keyframe) {
            pthread_mutex_unlock(&cl->lock);
            return;
        }
        if (type == BS_MSG_VIDEO)
            cl->want_key = 0;
    }

    if (cl->queued + body_len > BS_QUEUE_MAX_BYTES) {
        client_purge(cl);
        cl->want_key = 1;
        pthread_mutex_unlock(&cl->lock);
        bs_encoder_request_keyframe(cl->srv->enc);
        if (!cl->srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: %s fell behind, resynchronising\n",
                    bs_conn_peer(cl->conn));
        return;
    }

    BsPacket *p = malloc(sizeof(*p) + body_len);
    if (!p) {
        pthread_mutex_unlock(&cl->lock);
        return;
    }
    p->next     = NULL;
    p->type     = type;
    p->keyframe = (uint8_t)keyframe;
    p->head_len = (uint8_t)head_len;
    if (head_len)
        memcpy(p->head, head, head_len);
    p->len = body_len;
    if (body_len)
        memcpy(p->data, body, body_len);

    if (cl->tail)
        cl->tail->next = p;
    else
        cl->head = p;
    cl->tail = p;
    cl->queued += body_len;

    pthread_cond_signal(&cl->cond);
    pthread_mutex_unlock(&cl->lock);
}

static void *client_send_thread(void *arg)
{
    BsClient *cl = arg;

    while (!cl->srv->stop && !cl->gone) {
        pthread_mutex_lock(&cl->lock);
        while (!cl->head && !cl->gone && !cl->srv->stop)
            pthread_cond_wait(&cl->cond, &cl->lock);
        BsPacket *p = cl->head;
        if (p) {
            cl->head = p->next;
            if (!cl->head)
                cl->tail = NULL;
            cl->queued -= p->len;
        }
        pthread_mutex_unlock(&cl->lock);

        if (!p)
            continue;

        int rc = bs_send_msg(cl->conn, p->type,
                             p->head_len ? p->head : NULL, p->head_len,
                             p->len ? p->data : NULL, p->len);
        if (p->type == BS_MSG_VIDEO)
            cl->frames_sent++;
        free(p);

        if (rc < 0) {
            cl->gone = 1;
            break;
        }
    }
    cl->gone = 1;
    return NULL;
}

/* ---------------------------------------------------------------- input */

/*
 * Buttons are merged across clients rather than overwritten.
 *
 * Two people on two phones can hold two different buttons, and a release
 * from one must not clear what the other is still pressing -- so the
 * source is told the OR of what everybody holds, and only when that OR
 * actually changes.
 */
static void client_button(BsClient *cl, BsButton code, int pressed)
{
    BsServer *srv = cl->srv;
    if ((int)code < 1 || (int)code > 31)
        return;
    uint32_t bit = 1u << ((int)code - 1);

    pthread_mutex_lock(&srv->roster);
    if (pressed)
        cl->buttons |= bit;
    else
        cl->buttons &= ~bit;

    uint32_t merged = 0;
    for (int i = 0; i < srv->max_clients; i++)
        if (srv->clients[i].in_use && !srv->clients[i].gone)
            merged |= srv->clients[i].buttons;

    int was = (srv->merged_buttons & bit) != 0;
    int now = (merged & bit) != 0;
    srv->merged_buttons = merged;
    pthread_mutex_unlock(&srv->roster);

    if (was != now && srv->source->button)
        srv->source->button(srv->source->self, code, now);
}

/*
 * A client that leaves while holding something must not leave it held.
 * Without this a disconnection in the middle of a jump is a permanently
 * pressed A button, which looks like the emulator has hung.
 */
static void client_release_all(BsClient *cl)
{
    BsServer *srv = cl->srv;

    pthread_mutex_lock(&srv->roster);
    uint32_t had = cl->buttons;
    cl->buttons = 0;
    uint32_t merged = 0;
    for (int i = 0; i < srv->max_clients; i++)
        if (srv->clients[i].in_use && !srv->clients[i].gone && &srv->clients[i] != cl)
            merged |= srv->clients[i].buttons;
    uint32_t released = srv->merged_buttons & ~merged & had;
    srv->merged_buttons = merged;
    pthread_mutex_unlock(&srv->roster);

    if (!released || !srv->source->button)
        return;
    for (int b = 1; b <= 31; b++)
        if (released & (1u << (b - 1)))
            srv->source->button(srv->source->self, (BsButton)b, 0);
}

/* ------------------------------------------------------------ handshake */

static int client_handshake(BsClient *cl)
{
    BsServer *srv = cl->srv;
    BsConn   *conn = cl->conn;

    BsHello hello;
    if (bs_read_exact(conn, &hello, sizeof(hello)) != 0 ||
        hello.magic != BS_MAGIC || hello.version != BS_VERSION) {
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: bad handshake from %s\n",
                    bs_conn_peer(conn));
        return -1;
    }

    size_t extra_size = 0;
    const uint8_t *extra = bs_encoder_extradata(srv->enc, &extra_size);

    BsHelloAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic    = BS_MAGIC;
    ack.version  = BS_VERSION;
    ack.accepted = 1;
    ack.console  = (uint8_t)srv->info.console;
    ack.codec    = BS_CODEC_H264;
    ack.width    = (uint16_t)srv->info.width;
    ack.height   = (uint16_t)srv->info.height;
    ack.fps      = (uint16_t)srv->info.fps;
    ack.extradata_size = (uint16_t)extra_size;
    if (srv->aenc) {
        ack.audio_codec    = BS_ACODEC_OPUS;
        ack.audio_channels = (uint8_t)srv->info.audio_channels;
        ack.audio_rate     = (uint16_t)48000;   /* what Opus actually carries */
    }

    if (bs_write_all(conn, &ack, sizeof(ack)) != 0)
        return -1;
    if (extra_size && bs_write_all(conn, extra, extra_size) != 0)
        return -1;
    return 0;
}

/*
 * The handshake happens on the client's own thread, not the accept loop.
 * A client that connects and then says nothing would otherwise hold the
 * door shut for everyone behind it.
 */
static void *client_recv_thread(void *arg)
{
    BsClient *cl = arg;
    BsServer *srv = cl->srv;
    uint8_t buf[512];

    if (client_handshake(cl) != 0) {
        cl->gone = 1;
        pthread_mutex_lock(&cl->lock);
        pthread_cond_signal(&cl->cond);
        pthread_mutex_unlock(&cl->lock);
        return NULL;
    }

    /* A client that has just connected has no reference picture and
     * decodes nothing until the next keyframe -- up to a second of blank
     * window at gop=fps. Ask for one now. */
    cl->want_key = 1;
    cl->ready = 1;
    bs_encoder_request_keyframe(srv->enc);

    /* Someone is watching, so the pump has work to do. */
    pthread_mutex_lock(&srv->roster);
    pthread_cond_broadcast(&srv->roster_cond);
    pthread_mutex_unlock(&srv->roster);

    if (!srv->cfg.quiet)
        fprintf(stderr, "bottom_screen: client %s ready\n", bs_conn_peer(cl->conn));

    while (!srv->stop && !cl->gone) {
        uint8_t type = 0;
        size_t n = 0;
        if (bs_recv_msg(cl->conn, &type, buf, sizeof(buf), &n) != 0)
            break;

        if (type == BS_MSG_INPUT && n >= sizeof(BsInputEvent)) {
            BsInputEvent ev;
            memcpy(&ev, buf, sizeof(ev));
            switch (ev.type) {
            case BS_INPUT_TOUCH_DOWN:
            case BS_INPUT_TOUCH_MOVE:
            case BS_INPUT_TOUCH_UP:
                /* One finger, one pointer: the most recent touch wins,
                 * whoever sent it. */
                if (srv->source->touch)
                    srv->source->touch(srv->source->self, ev.type, ev.x, ev.y);
                break;
            case BS_INPUT_BUTTON_DOWN:
            case BS_INPUT_BUTTON_UP:
                client_button(cl, (BsButton)ev.code,
                              ev.type == BS_INPUT_BUTTON_DOWN);
                break;
            case BS_INPUT_AXIS:
                /* A stick rests at zero, so the last word is the right
                 * one; merging two would fight rather than combine. */
                if (srv->source->axis)
                    srv->source->axis(srv->source->self, ev.code, ev.x);
                break;
            default:
                break;
            }
        } else if (type == BS_MSG_PING) {
            client_send(cl, BS_MSG_PONG, 0, NULL, 0, NULL, 0);
        } else if (type == BS_MSG_REQUEST_KEYFRAME) {
            bs_encoder_request_keyframe(srv->enc);
        } else if (type == BS_MSG_SET_QUALITY && n >= sizeof(BsQuality)) {
            BsQuality q;
            memcpy(&q, buf, sizeof(q));
            srv->pending_bitrate = (int)q.bitrate;
            srv->quality_dirty = 1;
        }
    }

    /* This client left. The server has not, and neither have the others. */
    cl->gone = 1;
    client_release_all(cl);
    pthread_mutex_lock(&cl->lock);
    pthread_cond_signal(&cl->cond);
    pthread_mutex_unlock(&cl->lock);
    return NULL;
}

/* ------------------------------------------------------------- roster */

static int server_live_clients(BsServer *srv)
{
    int n = 0;
    for (int i = 0; i < srv->max_clients; i++)
        if (srv->clients[i].in_use && !srv->clients[i].gone)
            n++;
    return n;
}

/*
 * Frees the slots of clients that have finished. Their threads are
 * joined here rather than detached, so a shutdown never races a thread
 * that is still touching the server.
 */
static void server_reap(BsServer *srv)
{
    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (!cl->in_use || !cl->gone)
            continue;

        pthread_mutex_lock(&cl->lock);
        pthread_cond_broadcast(&cl->cond);
        pthread_mutex_unlock(&cl->lock);
        if (cl->conn)
            shutdown(bs_conn_fd(cl->conn), SHUT_RDWR);

        if (cl->recv_started) { pthread_join(cl->recv_tid, NULL); cl->recv_started = 0; }
        if (cl->send_started) { pthread_join(cl->send_tid, NULL); cl->send_started = 0; }

        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: client %s gone after %u frames\n",
                    bs_conn_peer(cl->conn), cl->frames_sent);

        client_purge(cl);
        pthread_cond_destroy(&cl->cond);
        pthread_mutex_destroy(&cl->lock);
        bs_conn_close(cl->conn);

        BsServer *keep = cl->srv;
        memset(cl, 0, sizeof(*cl));
        cl->srv = keep;
    }
}

/*
 * Tells a client it cannot come in, instead of just hanging up on it.
 *
 * A bare disconnection is indistinguishable from a crash, a firewall or
 * a wrong port, and someone holding a phone has no way to tell which. A
 * refused ack costs one round trip and turns all that into "the server
 * is full".
 *
 * The read is given a deadline because this runs on the accept thread:
 * a client that connects and never speaks must not hold the door shut
 * for the people behind it.
 */
static void client_refuse(BsConn *conn)
{
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    BsHello hello;
    if (bs_read_exact(conn, &hello, sizeof(hello)) != 0)
        return;

    BsHelloAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic    = BS_MAGIC;
    ack.version  = BS_VERSION;
    ack.accepted = 0;
    bs_write_all(conn, &ack, sizeof(ack));
}

static int server_adopt(BsServer *srv, BsConn *conn)
{
    server_reap(srv);

    BsClient *cl = NULL;
    for (int i = 0; i < srv->max_clients; i++)
        if (!srv->clients[i].in_use) { cl = &srv->clients[i]; break; }

    if (!cl) {
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: no room for %s (%d clients)\n",
                    bs_conn_peer(conn), srv->max_clients);
        client_refuse(conn);
        return -1;
    }

    cl->srv  = srv;
    cl->conn = conn;
    cl->gone = 0;
    cl->ready = 0;
    pthread_mutex_init(&cl->lock, NULL);
    pthread_cond_init(&cl->cond, NULL);

    pthread_mutex_lock(&srv->roster);
    cl->in_use = 1;
    pthread_mutex_unlock(&srv->roster);

    if (pthread_create(&cl->send_tid, NULL, client_send_thread, cl) == 0)
        cl->send_started = 1;
    if (pthread_create(&cl->recv_tid, NULL, client_recv_thread, cl) == 0)
        cl->recv_started = 1;

    if (!cl->recv_started || !cl->send_started) {
        cl->gone = 1;
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------- encoder */

/*
 * Rebuilds the encoder at the requested bitrate. Called only from the
 * video loop, between frames.
 *
 * A failed rebuild keeps the old encoder rather than leaving the server
 * with none: a bitrate the encoder would not take should cost the person
 * their setting, not their picture.
 */
static int encoder_rebuild(BsServer *srv, int bitrate)
{
    BsEncoderConfig ecfg = {
        .width   = srv->info.width,
        .height  = srv->info.height,
        .fps     = srv->info.fps,
        .bitrate = bitrate,
        .gop     = srv->cfg.gop,
        .pixfmt  = srv->info.pixfmt,
        .encoder = srv->cfg.encoder,
    };

    char err[128] = "";
    BsEncoder *fresh = bs_encoder_create(&ecfg, err, sizeof(err));
    if (!fresh) {
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: keeping the old encoder: %s\n", err);
        return -1;
    }

    BsEncoder *old = srv->enc;
    srv->enc = fresh;
    bs_encoder_destroy(old);
    bs_encoder_request_keyframe(srv->enc);
    return 0;
}

/* ----------------------------------------------------------- broadcast */

typedef struct {
    BsServer *srv;
    uint32_t  frame_id;
    uint32_t  timestamp_us;
    uint64_t  bytes;
} SendCtx;

static void on_encoded(const uint8_t *data, size_t size, int keyframe, void *user)
{
    SendCtx *s = user;
    BsServer *srv = s->srv;

    BsVideoHeader vh;
    memset(&vh, 0, sizeof(vh));
    vh.frame_id       = s->frame_id;
    vh.timestamp_us   = s->timestamp_us;
    vh.fragment_id    = 0;
    vh.fragment_count = 1;   /* TCP never fragments; UDP will */
    vh.flags          = BS_VFLAG_END_OF_FRAME | (keyframe ? BS_VFLAG_KEYFRAME : 0);

    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone)
            client_send(cl, BS_MSG_VIDEO, keyframe, &vh, sizeof(vh), data, size);
    }
    s->bytes += size;
}

typedef struct {
    BsServer *srv;
} AudioCtx;

static void on_audio(const uint8_t *data, size_t size, void *user)
{
    AudioCtx *a = user;
    BsServer *srv = a->srv;

    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (!cl->in_use || cl->gone)
            continue;
        BsAudioHeader ah;
        ah.timestamp_us = bs_now_us();
        ah.sequence     = cl->audio_seq++;
        client_send(cl, BS_MSG_AUDIO, 0, &ah, sizeof(ah), data, size);
    }
}

static void broadcast_stream_info(BsServer *srv, uint32_t from_frame_id)
{
    BsStreamInfo si;
    si.from_frame_id = from_frame_id;
    si.width  = (uint16_t)srv->info.width;
    si.height = (uint16_t)srv->info.height;
    si.fps    = (uint16_t)srv->info.fps;

    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone) {
            client_send(cl, BS_MSG_STREAM_INFO, 0, &si, sizeof(si), NULL, 0);
            /* The size changed under it, so its reference picture is
             * worthless whatever it was. */
            cl->want_key = 1;
        }
    }
}

/* ------------------------------------------------------------ the pump */

/*
 * One loop, however many clients. It idles while nobody is watching --
 * an emulator should not pay for an encoder that feeds no one.
 */
static void *pump_thread(void *arg)
{
    BsServer *srv = arg;

    SendCtx  sc = { .srv = srv, .frame_id = 0, .timestamp_us = 0, .bytes = 0 };
    AudioCtx ac = { .srv = srv };
    uint32_t started = bs_now_us();

    /* One video frame's worth of sound is the natural drain size: the
     * loop already runs once per frame, and asking for more would only
     * add latency waiting to fill it. */
    const int audio_chunk = srv->aenc && srv->info.audio_rate > 0
                          ? srv->info.audio_rate / (srv->info.fps > 0 ? srv->info.fps : 30) + 64
                          : 0;
    int16_t *audio_buf = NULL;
    if (audio_chunk > 0)
        audio_buf = malloc((size_t)audio_chunk * srv->info.audio_channels * sizeof(int16_t));

    while (!srv->stop) {
        pthread_mutex_lock(&srv->roster);
        while (!srv->stop && server_live_clients(srv) == 0)
            pthread_cond_wait(&srv->roster_cond, &srv->roster);
        pthread_mutex_unlock(&srv->roster);
        if (srv->stop)
            break;

        if (srv->quality_dirty) {
            srv->quality_dirty = 0;
            if (encoder_rebuild(srv, srv->pending_bitrate) == 0 && !srv->cfg.quiet)
                fprintf(stderr, "bottom_screen: bitrate now %d bit/s\n",
                        srv->pending_bitrate);
        }

        /*
         * The source may have changed shape -- someone raised the
         * emulator's internal resolution. Rebuild the encoder and tell
         * the clients, rather than dropping connections over a setting.
         */
        BsSourceInfo now;
        srv->source->get_info(srv->source->self, &now);
        if (now.width != srv->info.width || now.height != srv->info.height) {
            srv->info.width = now.width;
            srv->info.height = now.height;
            if (encoder_rebuild(srv, srv->cfg.bitrate) == 0) {
                broadcast_stream_info(srv, sc.frame_id);
                if (!srv->cfg.quiet)
                    fprintf(stderr, "bottom_screen: now %dx%d\n",
                            srv->info.width, srv->info.height);
            }
        }

        int stride = 0;
        uint32_t ts = 0;
        const uint8_t *pixels = srv->source->acquire(srv->source->self, &stride, &ts);
        if (!pixels)
            break;

        sc.timestamp_us = ts;
        if (bs_encoder_encode(srv->enc, pixels, stride, on_encoded, &sc) < 0) {
            if (!srv->cfg.quiet)
                fprintf(stderr, "bottom_screen: encode failed\n");
            break;
        }
        sc.frame_id++;
        srv->frames++;

        /* Sound goes out alongside, in its own messages. A client that
         * cannot keep up drops one without disturbing the other. */
        if (audio_buf && srv->source->take_audio) {
            int got = srv->source->take_audio(srv->source->self,
                                              audio_buf, audio_chunk);
            if (got > 0)
                bs_audio_encode(srv->aenc, audio_buf, got, on_audio, &ac);
        }

        if (!srv->cfg.quiet && srv->info.fps > 0 &&
            sc.frame_id % (uint32_t)(srv->info.fps * 5) == 0) {
            uint32_t elapsed = bs_now_us() - started;
            double mbps = elapsed ? (double)sc.bytes * 8.0 / elapsed : 0.0;
            printf("bottom_screen: %u frames, %.2f Mbit/s, %d client(s)\n",
                   sc.frame_id, mbps, server_live_clients(srv));
            fflush(stdout);
        }
    }

    free(audio_buf);
    return NULL;
}

static void *accept_thread(void *arg)
{
    BsServer *srv = arg;
    char err[256];

    while (!srv->stop) {
        BsConn *conn = bs_accept(srv->listen_fd, err, sizeof(err));
        if (!conn) {
            if (srv->stop)
                break;
            continue;
        }
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: client %s connected\n", bs_conn_peer(conn));

        if (server_adopt(srv, conn) != 0)
            bs_conn_close(conn);
    }
    return NULL;
}

/* ------------------------------------------------------------ lifecycle */

BsServer *bs_server_create(BsSource *source, const BsServerConfig *cfg,
                           char *err, size_t errlen)
{
    if (!source || !source->acquire || !source->get_info) {
        if (err) snprintf(err, errlen, "invalid source");
        return NULL;
    }

    BsServer *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        if (err) snprintf(err, errlen, "out of memory");
        return NULL;
    }
    srv->source = source;
    srv->listen_fd = -1;
    if (cfg)
        srv->cfg = *cfg;
    source->get_info(source->self, &srv->info);

    srv->max_clients = srv->cfg.max_clients > 0 ? srv->cfg.max_clients
                                                : BS_CLIENTS_DEFAULT;
    srv->clients = calloc((size_t)srv->max_clients, sizeof(*srv->clients));
    if (!srv->clients) {
        if (err) snprintf(err, errlen, "out of memory");
        goto fail;
    }
    pthread_mutex_init(&srv->roster, NULL);
    pthread_cond_init(&srv->roster_cond, NULL);

    BsEncoderConfig ecfg = {
        .width   = srv->info.width,
        .height  = srv->info.height,
        .fps     = srv->info.fps,
        .bitrate = srv->cfg.bitrate,
        .gop     = srv->cfg.gop,
        .pixfmt  = srv->info.pixfmt,
        .encoder = srv->cfg.encoder,
    };
    srv->enc = bs_encoder_create(&ecfg, err, errlen);
    if (!srv->enc)
        goto fail;

    if (srv->info.audio_rate > 0 && srv->info.audio_channels > 0) {
        BsAudioConfig acfg = {
            .rate = srv->info.audio_rate,
            .channels = srv->info.audio_channels,
            .bitrate = 0,
        };
        char aerr[128] = "";
        srv->aenc = bs_audio_create(&acfg, aerr, sizeof(aerr));
        if (!srv->aenc && !srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: no sound (%s)\n", aerr);
    }

    /*
     * Take the next free port rather than refusing to start.
     *
     * Three emulators can be running at once, and each wants a server.
     * Failing because a sibling got there first would mean the second
     * one silently has no stream, which is a confusing thing to debug
     * from a phone. The port actually bound is announced, and
     * bs_server_port reports it.
     */
    uint16_t wanted = srv->cfg.port ? srv->cfg.port : BS_DEFAULT_PORT;
    for (int i = 0; i < 20; i++) {
        uint16_t try_port = (uint16_t)(wanted + i);
        srv->listen_fd = bs_listen(try_port, err, errlen);
        if (srv->listen_fd >= 0) {
            srv->port = try_port;
            break;
        }
    }
    if (srv->listen_fd < 0)
        goto fail;

    if (pthread_create(&srv->pump_thread, NULL, pump_thread, srv) != 0) {
        if (err) snprintf(err, errlen, "cannot start the video thread");
        goto fail;
    }
    srv->pump_started = 1;

    if (pthread_create(&srv->accept_thread, NULL, accept_thread, srv) != 0) {
        if (err) snprintf(err, errlen, "cannot start the accept thread");
        goto fail;
    }
    srv->accept_started = 1;

    /*
     * stderr, not stdout: an emulator's stdout is usually redirected to
     * a file and block-buffered, so this line would sit unseen in a
     * buffer for minutes. It matters most exactly when it is surprising
     * -- the port asked for was taken and the server moved -- and a
     * message nobody sees is the same as no message.
     */
    if (!srv->cfg.quiet)
        fprintf(stderr, "bottom_screen: %dx%d @ %d fps, %s, listening on port %u "
                        "(up to %d clients)\n",
                srv->info.width, srv->info.height, srv->info.fps,
                bs_encoder_name(srv->enc), (unsigned)srv->port, srv->max_clients);
    return srv;

fail:
    bs_server_destroy(srv);
    return NULL;
}

void bs_server_stop(BsServer *srv)
{
    if (!srv || srv->stop)
        return;
    srv->stop = 1;

    /* The threads may be blocked in four different places, so break all
     * of them: accept on the listening socket, a read on each client, a
     * client's send queue, and acquire on a source that only returns
     * when a frame arrives. */
    if (srv->listen_fd >= 0)
        shutdown(srv->listen_fd, SHUT_RDWR);

    if (srv->clients) {
        for (int i = 0; i < srv->max_clients; i++) {
            BsClient *cl = &srv->clients[i];
            if (!cl->in_use)
                continue;
            cl->gone = 1;
            if (cl->conn)
                shutdown(bs_conn_fd(cl->conn), SHUT_RDWR);
            pthread_mutex_lock(&cl->lock);
            pthread_cond_broadcast(&cl->cond);
            pthread_mutex_unlock(&cl->lock);
        }
    }

    pthread_mutex_lock(&srv->roster);
    pthread_cond_broadcast(&srv->roster_cond);
    pthread_mutex_unlock(&srv->roster);

    if (srv->source && srv->source->unblock)
        srv->source->unblock(srv->source->self);

    if (srv->accept_started) {
        pthread_join(srv->accept_thread, NULL);
        srv->accept_started = 0;
    }
    if (srv->pump_started) {
        pthread_join(srv->pump_thread, NULL);
        srv->pump_started = 0;
    }
    if (srv->clients)
        server_reap(srv);
}

void bs_server_destroy(BsServer *srv)
{
    if (!srv)
        return;
    bs_server_stop(srv);
    if (srv->listen_fd >= 0)
        close(srv->listen_fd);
    if (srv->enc)
        bs_encoder_destroy(srv->enc);
    if (srv->aenc)
        bs_audio_destroy(srv->aenc);
    if (srv->clients) {
        pthread_cond_destroy(&srv->roster_cond);
        pthread_mutex_destroy(&srv->roster);
        free(srv->clients);
    }
    free(srv);
}

uint16_t bs_server_port(const BsServer *srv) { return srv ? srv->port : 0; }
uint32_t bs_server_frames(const BsServer *srv) { return srv ? srv->frames : 0; }

int bs_server_has_client(const BsServer *srv)
{
    return bs_server_clients(srv) > 0;
}

int bs_server_clients(const BsServer *srv)
{
    if (!srv || !srv->clients)
        return 0;
    int n = 0;
    for (int i = 0; i < srv->max_clients; i++)
        if (srv->clients[i].in_use && !srv->clients[i].gone)
            n++;
    return n;
}
