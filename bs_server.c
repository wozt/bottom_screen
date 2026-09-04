#include "bs_server.h"
#include "bs_encoder.h"
#include "bs_net.h"
#include "bs_protocol.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct BsServer {
    BsSource      *source;
    BsSourceInfo   info;
    BsEncoder     *enc;
    BsServerConfig cfg;

    int       listen_fd;
    pthread_t thread;
    int       thread_started;

    volatile int stop;
    volatile int has_client;
    volatile uint32_t frames;

    /*
     * A quality change arrives on the input thread but is applied by the
     * video loop, between two frames. Swapping the encoder from under a
     * bs_encoder_encode already in progress would be a use-after-free
     * with a very confusing crash.
     */
    volatile int pending_bitrate;
    volatile int quality_dirty;

    /* The live connection, so stop() can break a blocking read. */
    BsConn *volatile conn;
};

typedef struct {
    BsServer *srv;
    BsConn   *conn;
    volatile int gone;
} InputArgs;

/*
 * Input runs on its own thread rather than being polled from the video
 * loop. The video loop spends most of its time waiting for the next
 * frame; reading input there would hold every event until that wait
 * ended -- up to 16 ms at 60 Hz, added to the thing a player feels most.
 */
static void *input_thread(void *arg)
{
    InputArgs *a = arg;
    BsSource *source = a->srv->source;
    uint8_t buf[512];

    while (!a->srv->stop && !a->gone) {
        uint8_t type = 0;
        long n = bs_recv_msg(a->conn, &type, buf, sizeof(buf));
        if (n <= 0)
            break;

        if (type == BS_MSG_INPUT && (size_t)n >= sizeof(BsInputEvent)) {
            BsInputEvent ev;
            memcpy(&ev, buf, sizeof(ev));
            switch (ev.type) {
            case BS_INPUT_TOUCH_DOWN:
            case BS_INPUT_TOUCH_MOVE:
            case BS_INPUT_TOUCH_UP:
                if (source->touch)
                    source->touch(source->self, ev.type, ev.x, ev.y);
                break;
            case BS_INPUT_BUTTON_DOWN:
            case BS_INPUT_BUTTON_UP:
                if (source->button)
                    source->button(source->self, ev.code,
                                   ev.type == BS_INPUT_BUTTON_DOWN);
                break;
            case BS_INPUT_AXIS:
                if (source->axis)
                    source->axis(source->self, ev.code, ev.x);
                break;
            default:
                break;
            }
        } else if (type == BS_MSG_PING) {
            bs_send_msg(a->conn, BS_MSG_PONG, NULL, 0, NULL, 0);
        } else if (type == BS_MSG_REQUEST_KEYFRAME) {
            bs_encoder_request_keyframe(a->srv->enc);
        } else if (type == BS_MSG_SET_QUALITY && (size_t)n >= sizeof(BsQuality)) {
            BsQuality q;
            memcpy(&q, buf, sizeof(q));
            a->srv->pending_bitrate = (int)q.bitrate;
            a->srv->quality_dirty = 1;
        }
    }
    /* This client left. The server has not. */
    a->gone = 1;
    return NULL;
}

typedef struct {
    BsConn  *conn;
    uint32_t frame_id;
    uint32_t timestamp_us;
    int      failed;
    uint64_t bytes;
} SendCtx;

static void on_encoded(const uint8_t *data, size_t size, int keyframe, void *user)
{
    SendCtx *s = user;
    if (s->failed)
        return;

    BsVideoHeader vh;
    memset(&vh, 0, sizeof(vh));
    vh.frame_id       = s->frame_id;
    vh.timestamp_us   = s->timestamp_us;
    vh.fragment_id    = 0;
    vh.fragment_count = 1;   /* TCP never fragments; UDP will */
    vh.flags          = BS_VFLAG_END_OF_FRAME | (keyframe ? BS_VFLAG_KEYFRAME : 0);

    if (bs_send_msg(s->conn, BS_MSG_VIDEO, &vh, sizeof(vh), data, size) < 0)
        s->failed = 1;
    else
        s->bytes += size;
}

/*
 * Rebuilds the encoder at the requested bitrate. Called only from the
 * video loop, between frames.
 *
 * A failed rebuild keeps the old encoder rather than leaving the server
 * with none: a bitrate the encoder would not take should cost the person
 * their setting, not their picture.
 */
static void apply_quality(BsServer *srv)
{
    BsEncoderConfig ecfg = {
        .width   = srv->info.width,
        .height  = srv->info.height,
        .fps     = srv->info.fps,
        .bitrate = srv->pending_bitrate,
        .gop     = srv->cfg.gop,
        .pixfmt  = srv->info.pixfmt,
        .encoder = srv->cfg.encoder,
    };

    char err[128] = "";
    BsEncoder *fresh = bs_encoder_create(&ecfg, err, sizeof(err));
    if (!fresh) {
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: keeping the old encoder: %s\n", err);
        return;
    }

    BsEncoder *old = srv->enc;
    srv->enc = fresh;
    bs_encoder_destroy(old);
    bs_encoder_request_keyframe(srv->enc);

    if (!srv->cfg.quiet)
        printf("bottom_screen: bitrate now %d bit/s\n", srv->pending_bitrate);
}

static void serve_client(BsServer *srv, BsConn *conn)
{
    BsHello hello;
    if (bs_read_exact(conn, &hello, sizeof(hello)) != 0 ||
        hello.magic != BS_MAGIC || hello.version != BS_VERSION) {
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: bad handshake from %s\n",
                    bs_conn_peer(conn));
        return;
    }

    size_t extra_size = 0;
    const uint8_t *extra = bs_encoder_extradata(srv->enc, &extra_size);

    BsHelloAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic   = BS_MAGIC;
    ack.version = BS_VERSION;
    ack.accepted = 1;
    ack.console = (uint8_t)srv->info.console;
    ack.codec   = BS_CODEC_H264;
    ack.width   = (uint16_t)srv->info.width;
    ack.height  = (uint16_t)srv->info.height;
    ack.fps     = (uint16_t)srv->info.fps;
    ack.extradata_size = (uint16_t)extra_size;

    if (bs_write_all(conn, &ack, sizeof(ack)) != 0)
        return;
    if (extra_size && bs_write_all(conn, extra, extra_size) != 0)
        return;

    InputArgs ia = { .srv = srv, .conn = conn, .gone = 0 };
    pthread_t tid;
    int have_thread = (pthread_create(&tid, NULL, input_thread, &ia) == 0);

    /* A client that has just connected has no reference picture and
     * decodes nothing until the next keyframe -- up to a second of blank
     * window at gop=fps. Ask for one now. */
    bs_encoder_request_keyframe(srv->enc);

    SendCtx sc = { .conn = conn, .frame_id = 0, .failed = 0, .bytes = 0 };
    uint32_t started = bs_now_us();

    while (!srv->stop && !sc.failed && !ia.gone) {
        if (srv->quality_dirty) {
            srv->quality_dirty = 0;
            apply_quality(srv);
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

        if (!srv->cfg.quiet &&
            sc.frame_id % (uint32_t)(srv->info.fps * 5) == 0) {
            uint32_t elapsed = bs_now_us() - started;
            double mbps = elapsed ? (double)sc.bytes * 8.0 / elapsed : 0.0;
            printf("bottom_screen: %u frames, %.2f Mbit/s\n", sc.frame_id, mbps);
            fflush(stdout);
        }
    }

    ia.gone = 1;
    if (have_thread) {
        shutdown(bs_conn_fd(conn), SHUT_RDWR);
        pthread_join(tid, NULL);
    }
    if (!srv->cfg.quiet)
        printf("bottom_screen: client %s gone after %u frames\n",
               bs_conn_peer(conn), sc.frame_id);
}

static void *server_thread(void *arg)
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
            printf("bottom_screen: client %s connected\n", bs_conn_peer(conn));

        srv->conn = conn;
        srv->has_client = 1;
        serve_client(srv, conn);
        srv->has_client = 0;
        srv->conn = NULL;
        bs_conn_close(conn);
    }
    return NULL;
}

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

    uint16_t port = srv->cfg.port ? srv->cfg.port : BS_DEFAULT_PORT;
    srv->listen_fd = bs_listen(port, err, errlen);
    if (srv->listen_fd < 0)
        goto fail;

    if (pthread_create(&srv->thread, NULL, server_thread, srv) != 0) {
        if (err) snprintf(err, errlen, "cannot start server thread");
        goto fail;
    }
    srv->thread_started = 1;

    if (!srv->cfg.quiet)
        printf("bottom_screen: %dx%d @ %d fps, %s, listening on port %u\n",
               srv->info.width, srv->info.height, srv->info.fps,
               bs_encoder_name(srv->enc), (unsigned)port);
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

    /* The thread may be blocked in three different places, so break all
     * of them: accept on the listening socket, a read on the client, and
     * acquire on a source that only returns when a frame arrives. */
    if (srv->listen_fd >= 0)
        shutdown(srv->listen_fd, SHUT_RDWR);
    if (srv->conn)
        shutdown(bs_conn_fd(srv->conn), SHUT_RDWR);
    if (srv->source && srv->source->unblock)
        srv->source->unblock(srv->source->self);

    if (srv->thread_started) {
        pthread_join(srv->thread, NULL);
        srv->thread_started = 0;
    }
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
    free(srv);
}

int bs_server_has_client(const BsServer *srv) { return srv ? srv->has_client : 0; }
uint32_t bs_server_frames(const BsServer *srv) { return srv ? srv->frames : 0; }
