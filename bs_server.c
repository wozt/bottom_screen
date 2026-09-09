#include "bs_server.h"
#include "bs_audio.h"
#include "bs_encoder.h"
#include "bs_net.h"
#include "bs_protocol.h"
#include "bs_ws.h"

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
    int       is_web;      /* a browser, framed in WebSocket rather than raw */
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

    /*
     * Which screen this one is watching. Per client, unlike the size and
     * the bitrate: two people looking at two different screens is the
     * whole point of the setting, and it is why this one is allowed its
     * own encoder where those two are not.
     */
    volatile int screen;   /* BsScreen */
} BsClient;

/*
 * One screen being sent: its pictures, its encoder, its thread.
 *
 * Everything here used to live directly in BsServer, back when there was
 * only ever one picture to send. A second screen needs a second encoder
 * -- two different pictures cannot come out of one -- so what was shared
 * between clients is now shared between the clients watching the same
 * screen.
 *
 * The size and the bitrate stay shared within a stream, for the reason
 * they always were: one encoder, one answer, last asker wins. The screen
 * is the one setting that escapes that, because sharing it would mean
 * there was nothing to choose.
 */
typedef struct BsStream {
    BsServer    *srv;
    int          which;        /* BsScreen */
    /*
     * Whether this screen exists at all, which is not the same as
     * whether its pixels have started arriving.
     *
     * A backend that produces a screen only while somebody is watching
     * it cannot hand over a source before anybody has asked -- and
     * nobody can ask for a screen the server says it does not have.
     * That circle cost an evening: the mask said "bottom only" against a
     * real emulator, for ever. So a backend says it means to provide the
     * screen, clients may then choose it, and the source turns up on the
     * first frame after that.
     */
    int          offered;
    BsSource    *source;       /* NULL until the backend hands one over */
    BsSourceInfo info;
    BsEncoder   *enc;          /* built on demand; see stream_idle */

    pthread_t    tid;
    int          started;

    volatile int pending_bitrate;
    volatile int quality_dirty;
    volatile int bitrate_now;
    volatile int want_w, want_h;
    volatile int size_dirty;

    int          out_w, out_h;
    int          told_w, told_h;

    uint32_t     frame_id;
    uint64_t     bytes;

    /*
     * One keyframe, asked for by the server rather than by a client.
     *
     * Clients may not ask: one that is struggling asks constantly, which
     * is exactly when the others can least afford it, and three of them
     * turned the stream into mostly keyframes. But there are two moments
     * where waiting up to a second for the next one is a second of green
     * mush, and the server knows both without being told -- somebody
     * joining a stream that is already running, and somebody moving
     * between the two screens.
     *
     * Read and cleared by the pump, so the encoder is only ever touched
     * by the thread that owns it, and a burst of arrivals costs one
     * keyframe rather than one each.
     */
    volatile int want_keyframe;
} BsStream;

struct BsServer {
    /*
     * The source input and sound come from, which is the bottom screen's
     * -- video[BS_SCREEN_BOTTOM].source, kept here as well because
     * neither of those belongs to a picture. A client watching the top
     * screen still presses buttons, and there is one set of speakers
     * whatever you are looking at.
     */
    BsSource      *source;
    BsStream       video[BS_SCREEN_COUNT];
    BsAudioEncoder *aenc;      /* NULL when the source is silent */
    BsServerConfig cfg;

    int       listen_fd;
    uint16_t  port;          /* the one actually bound, not the one asked for */

    pthread_t accept_thread;
    int       accept_started;
    pthread_t audio_tid;
    int       audio_started;

    volatile int stop;
    volatile uint32_t frames;

    /* Which of a Wii U's two audio outputs to send. Shared between
     * clients because there is one sound, whatever is on screen; see
     * BS_MSG_SET_AUDIO_SOURCE. */
    volatile int audio_source;

    /*
     * The one question the machine is waiting on an answer to.
     *
     * One at a time, because a console shows one applet at a time -- a
     * 3DS asking for a name is not also asking for a Mii. The backend
     * puts a question here, every client is shown it, and the first
     * answer wins; a second is dropped because the id no longer
     * matches.
     *
     * Its own lock rather than the roster's: the answer arrives on a
     * client's receiving thread and is collected by the emulator's, and
     * neither has any business holding up the other's.
     */
    pthread_mutex_t prompt_lock;
    uint16_t  prompt_id;        /* 0 = nothing being asked */
    int       prompt_state;     /* 0 waiting, 1 answered, -1 cancelled */
    int       prompt_choice;
    char      prompt_answer[BS_PROMPT_MAX];
    uint16_t  prompt_next_id;

    BsClient *clients;
    int       max_clients;

    /* Guards the roster: in_use, gone, and the merged button state. */
    pthread_mutex_t roster;

    /* Held while reaping. Both the pump and the accept thread clean up
     * after departed clients, and two of them joining the same thread
     * is undefined behaviour rather than a wasted call. */
    pthread_mutex_t reap_lock;
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
        /* Waits for the next keyframe rather than asking for one: the
         * client that fell behind is not a reason to make everybody
         * else pay for a new one. */
        cl->want_key = 1;
        pthread_mutex_unlock(&cl->lock);
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

        int rc = cl->is_web
            ? bs_ws_send_msg(cl->conn, p->type,
                             p->head_len ? p->head : NULL, p->head_len,
                             p->len ? p->data : NULL, p->len)
            : bs_send_msg(cl->conn, p->type,
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

/*
 * Tells a client which screens this backend can actually produce, so it
 * can offer the choice or not offer it.
 *
 * A client that does not know this message skips it by its length and
 * carries on, which is exactly right: it never learns there is a top
 * screen, and it never had one before either.
 */
static void announce_screens(BsServer *srv);

static void client_send_screens(BsClient *cl)
{
    BsServer *srv = cl->srv;
    BsScreens sc;
    memset(&sc, 0, sizeof(sc));
    for (int i = 0; i < BS_SCREEN_COUNT; i++)
        if (srv->video[i].offered)
            sc.available |= (uint8_t)(1u << i);

    int watching[BS_SCREEN_COUNT] = { 0, 0 };
    for (int i = 0; i < srv->max_clients; i++) {
        const BsClient *o = &srv->clients[i];
        if (o->in_use && !o->gone && o->ready &&
            o->screen >= 0 && o->screen < BS_SCREEN_COUNT)
            watching[o->screen]++;
    }
    sc.watching_bottom = (uint8_t)watching[BS_SCREEN_BOTTOM];
    sc.watching_top    = (uint8_t)watching[BS_SCREEN_TOP];

    client_send(cl, BS_MSG_SCREENS, 0, &sc, sizeof(sc), NULL, 0);
}

/* Whether anybody other than `cl` is already watching that screen. */
static int others_watching(BsServer *srv, const BsClient *cl, int screen)
{
    for (int i = 0; i < srv->max_clients; i++) {
        const BsClient *o = &srv->clients[i];
        if (o != cl && o->in_use && !o->gone && o->ready && o->screen == screen)
            return 1;
    }
    return 0;
}

/*
 * Moves one client to the other screen.
 *
 * Nothing is sent to it until the new stream's next keyframe: what it
 * has been decoding is a different picture of a different size, and a P
 * frame from the new one applied to the old reference is garbage. The
 * stream it is joining may not even be running yet, so the pump is
 * woken to build an encoder; the one it left may now have nobody on it,
 * and that same wake-up is what lets that loop notice and stop.
 */
/*
 * Tells one client the shape of one stream.
 *
 * Separate from broadcast_stream_info because that only fires when the
 * size changes, which is the wrong rule for somebody arriving. A client
 * moving to a screen that is already running -- because another client
 * is watching it -- was never told anything at all: it kept the size
 * from its handshake, which is the bottom screen's, and decoded a 5:3
 * picture as though it were 4:3. That is one client's setting being
 * decided by whether a different client happened to be watching, which
 * is exactly the kind of fault that looks intermittent.
 */
static void client_send_stream_info(BsClient *cl, BsStream *st)
{
    if (st->out_w <= 0 || st->out_h <= 0)
        return;   /* not running yet; the pump announces when it starts */

    BsStreamInfo si;
    si.from_frame_id = st->frame_id;
    si.width  = (uint16_t)st->out_w;
    si.height = (uint16_t)st->out_h;
    si.fps    = (uint16_t)st->info.fps;
    client_send(cl, BS_MSG_STREAM_INFO, 0, &si, sizeof(si), NULL, 0);
}

static void client_set_screen(BsClient *cl, int screen)
{
    BsServer *srv = cl->srv;
    if (screen < 0 || screen >= BS_SCREEN_COUNT || screen == cl->screen)
        return;
    if (!srv->video[screen].offered)
        return;   /* asked for a screen this backend does not have */

    cl->screen = screen;
    cl->want_key = 1;
    /*
     * One keyframe on the stream being joined, so the picture comes back
     * now rather than at the next scheduled one. Only where somebody
     * else is already watching: if this client is the first, the encoder
     * is about to be built for it and its opening frame is a keyframe.
     */
    if (others_watching(srv, cl, screen))
        srv->video[screen].want_keyframe = 1;

    /*
     * Throw away what is queued for the screen it just left -- those are
     * pictures of somewhere else, and sending them costs the bandwidth
     * this switch was probably made to save.
     *
     * Under cl->lock, which client_purge requires and which is not
     * ceremony here. The sending thread takes a packet off the head
     * under that lock and releases it before writing to the socket, so a
     * purge without it can free the packet the socket is reading from.
     * That is not theoretical: it took thirty seconds of clients
     * switching screens to catch it.
     */
    pthread_mutex_lock(&cl->lock);
    client_purge(cl);
    pthread_mutex_unlock(&cl->lock);

    pthread_mutex_lock(&srv->roster);
    pthread_cond_broadcast(&srv->roster_cond);
    pthread_mutex_unlock(&srv->roster);

    /*
     * What it is about to receive, said to it alone. If that stream is
     * not running yet the pump says it when it starts; if it is already
     * running, this is the only chance there will be, because nothing is
     * about to change size.
     */
    client_send_stream_info(cl, &srv->video[screen]);

    if (!srv->cfg.quiet)
        fprintf(stderr, "bottom_screen: client %s moved to the %s screen\n",
                bs_conn_peer(cl->conn),
                screen == BS_SCREEN_TOP ? "top" : "bottom");
}

static int client_handshake(BsClient *cl)
{
    BsServer *srv = cl->srv;
    BsConn   *conn = cl->conn;

    /*
     * A browser has already said who it is: the HTTP upgrade was the
     * hello, and asking it to send another one over the socket it just
     * opened would be ceremony for its own sake. Everything after this
     * point is identical for both.
     */
    if (!cl->is_web) {
        BsHello hello;
        if (bs_read_exact(conn, &hello, sizeof(hello)) != 0 ||
            hello.magic != BS_MAGIC || hello.version != BS_VERSION) {
            if (!srv->cfg.quiet)
                fprintf(stderr, "bottom_screen: bad handshake from %s\n",
                        bs_conn_peer(conn));
            return -1;
        }
    }

    /*
     * A client always arrives on the bottom screen, so this is the one
     * that answers for it. Switching afterwards needs no new parameter
     * sets: the encoder repeats SPS/PPS in front of every keyframe, and
     * a client that has just changed screens is waiting for one anyway.
     */
    BsStream *st = &srv->video[BS_SCREEN_BOTTOM];
    size_t extra_size = 0;
    const uint8_t *extra = bs_encoder_extradata(st->enc, &extra_size);

    BsHelloAck ack;
    memset(&ack, 0, sizeof(ack));
    ack.magic    = BS_MAGIC;
    ack.version  = BS_VERSION;
    ack.accepted = 1;
    ack.console  = (uint8_t)st->info.console;
    ack.codec    = BS_CODEC_H264;
    /* What is on the wire, which is what a client has to draw and to
     * aim its touches in -- not what the emulator happens to render. */
    ack.width    = (uint16_t)st->out_w;
    ack.height   = (uint16_t)st->out_h;
    ack.fps      = (uint16_t)st->info.fps;
    ack.extradata_size = (uint16_t)extra_size;
    if (srv->aenc) {
        ack.audio_codec    = BS_ACODEC_OPUS;
        ack.audio_channels = (uint8_t)st->info.audio_channels;
        ack.audio_rate     = (uint16_t)48000;   /* what Opus actually carries */
    }

    if (cl->is_web) {
        /* One frame carrying the ack and the parameter sets together,
         * because a WebSocket delivers whole messages and splitting them
         * would make the page reassemble something it never had to. */
        uint8_t greeting[sizeof(ack) + 4096];
        if (extra_size > sizeof(greeting) - sizeof(ack))
            return -1;
        memcpy(greeting, &ack, sizeof(ack));
        if (extra_size)
            memcpy(greeting + sizeof(ack), extra, extra_size);
        if (bs_ws_send_raw(conn, greeting, sizeof(ack) + extra_size) != 0)
            return -1;
        return 0;
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

    /* A client that has just connected has no reference picture, so it
     * is sent nothing until the next keyframe -- a second at most, and
     * a blank window rather than a corrupt one. It used to ask for one,
     * which made every arrival cost every other client a keyframe. */
    cl->want_key = 1;
    cl->ready = 1;

    /*
     * After ready, not inside the handshake. Everything on the normal
     * path goes out through the sending thread, and client_send drops
     * whatever is handed to it before the client is ready -- so an
     * announcement made a few lines earlier was thrown away in silence,
     * and every client believed there was no top screen.
     */
    client_send_screens(cl);

    /*
     * A client arriving on a stream that is already running waits for
     * the next scheduled keyframe -- up to a second of nothing, or of
     * green, depending on the decoder. One is asked for instead. Alone
     * on a stream it needs nothing: the encoder is being built for it,
     * and its opening frame is a keyframe already.
     */
    if (others_watching(srv, cl, cl->screen))
        srv->video[cl->screen].want_keyframe = 1;

    /* And everyone else learns there is one more of them. */
    announce_screens(srv);

    /* Someone is watching, so the pump has work to do. */
    pthread_mutex_lock(&srv->roster);
    pthread_cond_broadcast(&srv->roster_cond);
    pthread_mutex_unlock(&srv->roster);

    if (!srv->cfg.quiet)
        fprintf(stderr, "bottom_screen: client %s ready\n", bs_conn_peer(cl->conn));

    while (!srv->stop && !cl->gone) {
        uint8_t type = 0;
        size_t n = 0;
        const int rc = cl->is_web
            ? bs_ws_recv_msg(cl->conn, &type, buf, sizeof(buf), &n)
            : bs_recv_msg(cl->conn, &type, buf, sizeof(buf), &n);
        if (rc != 0)
            break;

        if (type == BS_MSG_INPUT && n >= sizeof(BsInputEvent)) {
            BsInputEvent ev;
            memcpy(&ev, buf, sizeof(ev));
            switch (ev.type) {
            case BS_INPUT_TOUCH_DOWN:
            case BS_INPUT_TOUCH_MOVE:
            case BS_INPUT_TOUCH_UP:
                /*
                 * One finger, one pointer: the most recent touch wins,
                 * whoever sent it.
                 *
                 * Clients aim in the space that was announced to them,
                 * which is the encoded size. The backends work in the
                 * source's own space, so the conversion happens here --
                 * the one place that knows both numbers. Leaving it to
                 * the backends would put every tap wrong by exactly the
                 * scale, in three different files.
                 */
                /*
                 * Dropped outright from a client watching the top
                 * screen. There is no touch panel up there, and a tap
                 * arriving in that picture's coordinates would be scaled
                 * by the bottom screen's numbers and land somewhere
                 * arbitrary -- a stray press in the game rather than a
                 * missing one, which is the worse of the two. The
                 * clients do not draw a touch area in that mode either;
                 * this is the half of it that does not depend on every
                 * client being well behaved.
                 */
                if (srv->source->touch && cl->screen == BS_SCREEN_BOTTOM) {
                    BsStream *bot = &srv->video[BS_SCREEN_BOTTOM];
                    int tx = ev.x, ty = ev.y;
                    if (bot->out_w > 0 && bot->out_h > 0 &&
                        (bot->out_w != bot->info.width ||
                         bot->out_h != bot->info.height)) {
                        tx = ev.x * bot->info.width / bot->out_w;
                        ty = ev.y * bot->info.height / bot->out_h;
                    }
                    srv->source->touch(srv->source->self, ev.type, tx, ty);
                }
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
            /*
             * Ignored, deliberately.
             *
             * There is one encoder for everyone, so a keyframe asked for
             * by one client is paid for by all of them -- and a client
             * that is struggling asks constantly, which is exactly when
             * the others can least afford it. Three clients turned into
             * a stream that was mostly keyframes.
             *
             * A client that has nothing to decode waits for the next one
             * instead, which is a second away at most. The message is
             * still accepted and dropped rather than treated as an
             * error, because clients built before this went on sending
             * it.
             */
        } else if (type == BS_MSG_SET_SIZE && n >= sizeof(BsSize)) {
            BsSize sz;
            memcpy(&sz, buf, sizeof(sz));
            BsStream *st = &srv->video[cl->screen];
            st->want_w = sz.width;
            st->want_h = sz.height;
            st->size_dirty = 1;
        } else if (type == BS_MSG_SET_SCREEN && n >= sizeof(BsScreenChoice)) {
            BsScreenChoice sc;
            memcpy(&sc, buf, sizeof(sc));
            client_set_screen(cl, sc.screen);
        } else if (type == BS_MSG_PROMPT_REPLY && n >= sizeof(BsPromptReply)) {
            BsPromptReply rp;
            memcpy(&rp, buf, sizeof(rp));
            pthread_mutex_lock(&srv->prompt_lock);
            /*
             * Only if it answers the question still being asked. A
             * second person answering, or somebody answering one the
             * game has already withdrawn, is dropped here rather than
             * handed to whatever asked next.
             */
            if (srv->prompt_id != 0 && rp.id == srv->prompt_id &&
                srv->prompt_state == 0) {
                srv->prompt_state = rp.cancelled ? -1 : 1;
                srv->prompt_choice = rp.choice;
                size_t textlen = n - sizeof(rp);
                if (textlen >= sizeof(srv->prompt_answer))
                    textlen = sizeof(srv->prompt_answer) - 1;
                memcpy(srv->prompt_answer, buf + sizeof(rp), textlen);
                srv->prompt_answer[textlen] = '\0';
            }
            pthread_mutex_unlock(&srv->prompt_lock);
        } else if (type == BS_MSG_SET_AUDIO_SOURCE && n >= sizeof(BsAudioChoice)) {
            BsAudioChoice ac;
            memcpy(&ac, buf, sizeof(ac));
            if (ac.source <= BS_AUDIO_PAD)
                srv->audio_source = ac.source;
        } else if (type == BS_MSG_SET_QUALITY && n >= sizeof(BsQuality)) {
            BsQuality q;
            memcpy(&q, buf, sizeof(q));
            BsStream *st = &srv->video[cl->screen];
            st->pending_bitrate = (int)q.bitrate;
            st->quality_dirty = 1;
        }
    }

    /* This client left. The server has not, and neither have the others. */
    cl->gone = 1;
    client_release_all(cl);
    /* One fewer, which the others are told. */
    announce_screens(srv);
    pthread_mutex_lock(&cl->lock);
    pthread_cond_signal(&cl->cond);
    pthread_mutex_unlock(&cl->lock);

    /* Wake the pump so it can clean up after us. */
    pthread_mutex_lock(&srv->roster);
    pthread_cond_broadcast(&srv->roster_cond);
    pthread_mutex_unlock(&srv->roster);
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
    pthread_mutex_lock(&srv->reap_lock);
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
    pthread_mutex_unlock(&srv->reap_lock);
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

/*
 * Which protocol just knocked.
 *
 * A native client opens with BsHello, whose first four bytes spell the
 * magic; a browser opens with "GET ". They cannot be confused, so one
 * port serves both and there is no second port to explain or forward.
 *
 * A plain page request is answered here rather than in a client slot: a
 * browser fetches the page, then opens the socket, and the fetch has no
 * business occupying one of the four places.
 *
 * Returns 0 for a native client, 1 for a WebSocket, and -1 when the
 * connection is finished with.
 */
static int classify(BsServer *srv, BsConn *conn)
{
    /* A client that connects and says nothing must not hold the door
     * shut for the people behind it. */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    uint8_t first[4];
    ssize_t n = recv(bs_conn_fd(conn), first, sizeof(first), MSG_PEEK);
    if (n < (ssize_t)sizeof(first))
        return -1;

    if (!bs_ws_looks_like_http(first, sizeof(first))) {
        /* Back to blocking for the stream itself. */
        struct timeval none = { 0, 0 };
        setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
        setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
        return 0;
    }

    char err[128] = "";
    int rc = bs_ws_serve(conn, err, sizeof(err));
    if (rc == 1) {
        struct timeval none = { 0, 0 };
        setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
        setsockopt(bs_conn_fd(conn), SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));
        return 1;
    }
    if (rc < 0 && !srv->cfg.quiet)
        fprintf(stderr, "bottom_screen: %s from %s\n", err, bs_conn_peer(conn));
    return -1;
}

static int server_adopt(BsServer *srv, BsConn *conn, int is_web)
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
    cl->is_web = is_web;
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
static int encoder_rebuild(BsStream *st, int bitrate)
{
    BsServer *srv = st->srv;
    BsEncoderConfig ecfg = {
        .width      = st->info.width,
        .height     = st->info.height,
        .out_width  = st->want_w,
        .out_height = st->want_h,
        .fps        = st->info.fps,
        .bitrate    = bitrate,
        .gop        = srv->cfg.gop,
        .pixfmt     = st->info.pixfmt,
        .encoder    = srv->cfg.encoder,
    };

    char err[128] = "";
    BsEncoder *fresh = bs_encoder_create(&ecfg, err, sizeof(err));
    if (!fresh) {
        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: keeping the old encoder: %s\n", err);
        return -1;
    }

    BsEncoder *old = st->enc;
    st->enc = fresh;
    bs_encoder_destroy(old);
    bs_encoder_request_keyframe(st->enc);
    bs_encoder_out_size(st->enc, &st->out_w, &st->out_h);
    return 0;
}

/*
 * Nobody is watching this screen, so nothing should be encoding it.
 *
 * The bottom screen's encoder is built once and kept -- it is what the
 * handshake hands a client its parameter sets from, and it costs nothing
 * while the pump is asleep. The top screen's is built when somebody asks
 * for it and thrown away when the last of them leaves, because a second
 * encoder is real memory and a real thread's worth of work, and a
 * feature nobody is using should not be one of the machine's costs.
 */
static void stream_idle(BsStream *st)
{
    if (st->which == BS_SCREEN_BOTTOM || !st->enc)
        return;
    bs_encoder_destroy(st->enc);
    st->enc = NULL;
    st->told_w = st->told_h = 0;
}

/* ----------------------------------------------------------- broadcast */

typedef struct {
    BsStream *st;
    uint32_t  frame_id;
    uint32_t  timestamp_us;
} SendCtx;

static void on_encoded(const uint8_t *data, size_t size, int keyframe, void *user)
{
    SendCtx *s = user;
    BsStream *st = s->st;
    BsServer *srv = st->srv;

    BsVideoHeader vh;
    memset(&vh, 0, sizeof(vh));
    vh.frame_id       = s->frame_id;
    vh.timestamp_us   = s->timestamp_us;
    vh.fragment_id    = 0;
    vh.fragment_count = 1;   /* TCP never fragments; UDP will */
    vh.flags          = BS_VFLAG_END_OF_FRAME | (keyframe ? BS_VFLAG_KEYFRAME : 0);

    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->screen == st->which)
            client_send(cl, BS_MSG_VIDEO, keyframe, &vh, sizeof(vh), data, size);
    }
    st->bytes += size;
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

static void broadcast_stream_info(BsStream *st, uint32_t from_frame_id)
{
    BsServer *srv = st->srv;
    BsStreamInfo si;
    si.from_frame_id = from_frame_id;
    si.width  = (uint16_t)st->out_w;
    si.height = (uint16_t)st->out_h;
    si.fps    = (uint16_t)st->info.fps;

    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->screen == st->which) {
            client_send(cl, BS_MSG_STREAM_INFO, 0, &si, sizeof(si), NULL, 0);
            /* The size changed under it, so its reference picture is
             * worthless whatever it was. */
            cl->want_key = 1;
        }
    }
}

/*
 * Sound on its own clock.
 *
 * It used to be drained inside the video loop, one frame's worth per
 * frame, which sounds reasonable and is not: the loop spends the gap
 * between frames asleep in acquire(), so the sound came out in clumps
 * one frame apart. Measured against Cemu at 30fps, 39% of packets
 * arrived less than a millisecond after the one before and the median
 * gap was 33ms -- exactly the frame period. A client then has to smooth
 * 33ms of granularity, and when it cannot, that is the stutter.
 *
 * Here it is drained every couple of milliseconds regardless of what
 * the picture is doing, so packets leave at the 20ms cadence Opus
 * encodes them at rather than in bursts.
 *
 * It holds reap_lock while broadcasting because that is what stops a
 * client being freed underneath it: reaping happens on the pump thread,
 * and the two used to be the same thread.
 */
static void *audio_thread(void *arg)
{
    BsServer *srv = arg;
    const BsSourceInfo *info = &srv->video[BS_SCREEN_BOTTOM].info;
    if (!srv->aenc || !srv->source->take_audio || info->audio_rate <= 0)
        return NULL;

    /* 10ms per drain: small enough that nothing waits on it, large
     * enough that the loop is not the busiest thing on the machine. */
    const int chunk = info->audio_rate / 100 + 64;
    int16_t *buf = malloc((size_t)chunk * info->audio_channels * sizeof(int16_t));
    if (!buf)
        return NULL;

    AudioCtx ac = { .srv = srv };
    while (!srv->stop) {
        int got = srv->source->take_audio(srv->source->self, buf, chunk);
        if (got > 0) {
            pthread_mutex_lock(&srv->reap_lock);
            bs_audio_encode(srv->aenc, buf, got, on_audio, &ac);
            pthread_mutex_unlock(&srv->reap_lock);
        } else {
            struct timespec ts = { 0, 2 * 1000 * 1000 };   /* 2ms */
            nanosleep(&ts, NULL);
        }
    }
    free(buf);
    return NULL;
}

/* ------------------------------------------------------------ the pump */

/*
 * How many of them are watching this particular screen.
 *
 * A stream sleeps on its own count, not on the server's: with somebody
 * on the bottom screen and nobody on the top, the top must still be
 * idle. Called with the roster held.
 */
static int stream_live_clients(BsStream *st)
{
    BsServer *srv = st->srv;
    int n = 0;
    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->ready && cl->screen == st->which)
            n++;
    }
    return n;
}

/*
 * One loop per screen, however many clients. It idles while nobody is
 * watching that screen -- an emulator should not pay for an encoder
 * that feeds no one, and with two screens on offer that is the usual
 * case rather than the exception: almost nobody watches both.
 */
static void *pump_thread(void *arg)
{
    BsStream *st  = arg;
    BsServer *srv = st->srv;

    SendCtx  sc = { .st = st, .frame_id = 0, .timestamp_us = 0 };
    uint32_t started = bs_now_us();

    while (!srv->stop) {
        /*
         * Nobody watching: let go of whoever just left before settling
         * down to wait. Reaping only when the next client arrives would
         * park a socket and two threads for as long as nobody does --
         * which, on a machine serving one person, is for good.
         *
         * Both loops reap, and they have to: with somebody on the bottom
         * screen its loop never goes idle, so a client leaving the top
         * screen would lie there until the bottom emptied. server_reap
         * holds reap_lock across the whole sweep and clears in_use as it
         * goes, so the second caller finds nothing rather than joining a
         * thread twice.
         */
        pthread_mutex_lock(&srv->roster);
        while (!srv->stop && (stream_live_clients(st) == 0 || !st->source)) {
            pthread_mutex_unlock(&srv->roster);
            stream_idle(st);
            server_reap(srv);
            pthread_mutex_lock(&srv->roster);
            if (srv->stop || (stream_live_clients(st) > 0 && st->source))
                break;
            pthread_cond_wait(&srv->roster_cond, &srv->roster);
        }
        pthread_mutex_unlock(&srv->roster);
        if (srv->stop)
            break;

        /*
         * Somebody has just arrived on a screen that was asleep, so
         * there is no encoder yet. The bottom's is built at startup and
         * kept; this is the top's, and every setting it starts from is
         * whatever was last asked for on this screen.
         */
        if (!st->enc) {
            st->source->get_info(st->source->self, &st->info);
            if (encoder_rebuild(st, st->bitrate_now) != 0) {
                /* Nothing to encode with. Wait rather than spin: the
                 * client is still connected and may yet go elsewhere. */
                struct timespec ts = { 0, 100 * 1000 * 1000 };
                nanosleep(&ts, NULL);
                continue;
            }
            if (!srv->cfg.quiet)
                fprintf(stderr, "bottom_screen: encoding the %s screen at %dx%d\n",
                        st->which == BS_SCREEN_TOP ? "top" : "bottom",
                        st->out_w, st->out_h);
        }

        if (st->want_keyframe) {
            st->want_keyframe = 0;
            bs_encoder_request_keyframe(st->enc);
        }

        if (st->quality_dirty) {
            st->quality_dirty = 0;
            if (encoder_rebuild(st, st->pending_bitrate) == 0) {
                st->bitrate_now = st->pending_bitrate;
                if (!srv->cfg.quiet)
                    fprintf(stderr, "bottom_screen: bitrate now %d bit/s\n",
                            st->pending_bitrate);
            }
        }

        if (st->size_dirty) {
            st->size_dirty = 0;
            encoder_rebuild(st, st->bitrate_now);
        }

        /*
         * Announced by comparing with what was last said rather than by
         * whichever branch happened to rebuild. Either can change the
         * size -- they share one encoder -- so tying the announcement to
         * one of them loses it whenever the other got there first.
         */
        if (st->out_w != st->told_w || st->out_h != st->told_h) {
            st->told_w = st->out_w;
            st->told_h = st->out_h;
            broadcast_stream_info(st, sc.frame_id);
            if (!srv->cfg.quiet)
                fprintf(stderr, "bottom_screen: sending %dx%d from a %dx%d source\n",
                        st->out_w, st->out_h,
                        st->info.width, st->info.height);
        }

        /*
         * The source may have changed shape -- someone raised the
         * emulator's internal resolution. Rebuild the encoder and tell
         * the clients, rather than dropping connections over a setting.
         */
        BsSourceInfo now;
        st->source->get_info(st->source->self, &now);
        if (now.width != st->info.width || now.height != st->info.height) {
            st->info.width = now.width;
            st->info.height = now.height;
            if (encoder_rebuild(st, st->bitrate_now) == 0) {
                broadcast_stream_info(st, sc.frame_id);
                if (!srv->cfg.quiet)
                    fprintf(stderr, "bottom_screen: now %dx%d\n",
                            st->info.width, st->info.height);
            }
        }

        int stride = 0;
        uint32_t ts = 0;
        const uint8_t *pixels = st->source->acquire(st->source->self, &stride, &ts);
        if (!pixels)
            break;

        sc.timestamp_us = ts;
        if (bs_encoder_encode(st->enc, pixels, stride, on_encoded, &sc) < 0) {
            if (!srv->cfg.quiet)
                fprintf(stderr, "bottom_screen: encode failed\n");
            break;
        }
        sc.frame_id++;
        st->frame_id = sc.frame_id;
        if (st->which == BS_SCREEN_BOTTOM)
            srv->frames++;

        if (!srv->cfg.quiet && st->info.fps > 0 && st->which == BS_SCREEN_BOTTOM &&
            sc.frame_id % (uint32_t)(st->info.fps * 5) == 0) {
            uint32_t elapsed = bs_now_us() - started;
            double mbps = elapsed ? (double)st->bytes * 8.0 / elapsed : 0.0;
            printf("bottom_screen: %u frames, %.2f Mbit/s, %d client(s)\n",
                   sc.frame_id, mbps, server_live_clients(srv));
            fflush(stdout);
        }
    }

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
        const int kind = classify(srv, conn);
        if (kind < 0) {
            bs_conn_close(conn);
            continue;
        }

        if (!srv->cfg.quiet)
            fprintf(stderr, "bottom_screen: %s client %s connected\n",
                    kind == 1 ? "web" : "native", bs_conn_peer(conn));

        if (server_adopt(srv, conn, kind == 1) != 0)
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
    if (cfg) {
        srv->cfg = *cfg;
    }

    for (int i = 0; i < BS_SCREEN_COUNT; i++) {
        srv->video[i].srv   = srv;
        srv->video[i].which = i;
        srv->video[i].bitrate_now = srv->cfg.bitrate;
    }
    BsStream *bot = &srv->video[BS_SCREEN_BOTTOM];
    bot->source = source;
    bot->offered = 1;
    source->get_info(source->self, &bot->info);

    srv->max_clients = srv->cfg.max_clients > 0 ? srv->cfg.max_clients
                                                : BS_CLIENTS_DEFAULT;
    srv->clients = calloc((size_t)srv->max_clients, sizeof(*srv->clients));
    if (!srv->clients) {
        if (err) snprintf(err, errlen, "out of memory");
        goto fail;
    }
    pthread_mutex_init(&srv->roster, NULL);
    pthread_mutex_init(&srv->prompt_lock, NULL);
    pthread_mutex_init(&srv->reap_lock, NULL);
    pthread_cond_init(&srv->roster_cond, NULL);

    BsEncoderConfig ecfg = {
        .width   = bot->info.width,
        .height  = bot->info.height,
        .fps     = bot->info.fps,
        .bitrate = srv->cfg.bitrate,
        .gop     = srv->cfg.gop,
        .pixfmt  = bot->info.pixfmt,
        .encoder = srv->cfg.encoder,
    };
    bot->enc = bs_encoder_create(&ecfg, err, errlen);
    if (!bot->enc)
        goto fail;
    /* Nobody has asked for a size yet, so this is the source's -- but it
     * is read from the encoder rather than assumed, because that is
     * where rounding to even numbers happens. The handshake carries it
     * to every client, so it counts as already told. */
    bs_encoder_out_size(bot->enc, &bot->out_w, &bot->out_h);
    bot->told_w = bot->out_w;
    bot->told_h = bot->out_h;

    if (bot->info.audio_rate > 0 && bot->info.audio_channels > 0) {
        BsAudioConfig acfg = {
            .rate = bot->info.audio_rate,
            .channels = bot->info.audio_channels,
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

    /*
     * A loop per screen, both started now even though the top one may
     * never have a source or a viewer. It costs a thread asleep on a
     * condition variable, and starting one later would mean creating a
     * thread from inside a client's message handler -- a failure with
     * nowhere sensible to report itself.
     */
    for (int i = 0; i < BS_SCREEN_COUNT; i++) {
        if (pthread_create(&srv->video[i].tid, NULL, pump_thread,
                           &srv->video[i]) != 0) {
            if (err) snprintf(err, errlen, "cannot start the video thread");
            goto fail;
        }
        srv->video[i].started = 1;
    }

    if (srv->aenc &&
        pthread_create(&srv->audio_tid, NULL, audio_thread, srv) == 0)
        srv->audio_started = 1;

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
                bot->info.width, bot->info.height, bot->info.fps,
                bs_encoder_name(bot->enc), (unsigned)srv->port, srv->max_clients);
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

    for (int i = 0; i < BS_SCREEN_COUNT; i++) {
        BsSource *src = srv->video[i].source;
        if (src && src->unblock)
            src->unblock(src->self);
    }

    if (srv->accept_started) {
        pthread_join(srv->accept_thread, NULL);
        srv->accept_started = 0;
    }
    if (srv->audio_started) {
        pthread_join(srv->audio_tid, NULL);
        srv->audio_started = 0;
    }
    for (int i = 0; i < BS_SCREEN_COUNT; i++) {
        if (srv->video[i].started) {
            pthread_join(srv->video[i].tid, NULL);
            srv->video[i].started = 0;
        }
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
    for (int i = 0; i < BS_SCREEN_COUNT; i++)
        if (srv->video[i].enc)
            bs_encoder_destroy(srv->video[i].enc);
    if (srv->aenc)
        bs_audio_destroy(srv->aenc);
    if (srv->clients) {
        pthread_mutex_destroy(&srv->prompt_lock);
        pthread_cond_destroy(&srv->roster_cond);
        pthread_mutex_destroy(&srv->reap_lock);
        pthread_mutex_destroy(&srv->roster);
        free(srv->clients);
    }
    free(srv);
}

/*
 * Says the backend has a top screen, before it has one to hand over.
 *
 * Needed because the two facts are separate. A backend that reads its
 * top screen back off the GPU only while somebody is watching cannot
 * produce a source until somebody does -- and nobody can, if the server
 * only admits to screens it already holds a source for. So this
 * announces the intent; bs_server_set_top_source delivers on it, on the
 * first frame after a client asks.
 */
/* Tells everyone already connected what the screens are now, and wakes
 * the pumps -- one of them may have been waiting for exactly this. */
static void announce_screens(BsServer *srv)
{
    pthread_mutex_lock(&srv->roster);
    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->ready)
            client_send_screens(cl);
    }
    pthread_cond_broadcast(&srv->roster_cond);
    pthread_mutex_unlock(&srv->roster);
}

void bs_server_offer_top(BsServer *srv)
{
    if (!srv || srv->video[BS_SCREEN_TOP].offered)
        return;
    srv->video[BS_SCREEN_TOP].offered = 1;
    announce_screens(srv);
}

void bs_server_set_top_source(BsServer *srv, BsSource *top)
{
    if (!srv)
        return;
    BsStream *st = &srv->video[BS_SCREEN_TOP];
    st->source = top;
    st->offered = top != NULL;
    if (top)
        top->get_info(top->self, &st->info);

    announce_screens(srv);
}

int bs_server_wants_screen(const BsServer *srv, int screen)
{
    if (!srv || !srv->clients || screen < 0 || screen >= BS_SCREEN_COUNT)
        return 0;
    for (int i = 0; i < srv->max_clients; i++) {
        const BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->ready && cl->screen == screen)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ prompts */

uint16_t bs_server_prompt(BsServer *srv, int kind, const char *title,
                          const char *const *choices, int n_choices,
                          int max_len, int multiline)
{
    if (!srv || !title)
        return 0;
    if (n_choices < 0) n_choices = 0;
    if (n_choices > 255) n_choices = 255;

    /* Nobody watching, nobody to ask. The backend gets 0 and does
     * whatever it did before this existed, which is its own dialog on
     * whatever desktop it is running on. */
    if (bs_server_clients(srv) == 0)
        return 0;

    uint8_t body[BS_PROMPT_MAX];
    size_t at = 0;
    size_t n = strlen(title) + 1;
    if (n > sizeof(body)) n = sizeof(body);
    memcpy(body, title, n - 1);
    body[n - 1] = '\0';
    at = n;

    int sent_choices = 0;
    for (int i = 0; i < n_choices && choices; i++) {
        const char *c = choices[i] ? choices[i] : "";
        size_t len = strlen(c) + 1;
        if (at + len > sizeof(body))
            break;              /* a truncated list beats none at all */
        memcpy(body + at, c, len);
        at += len;
        sent_choices++;
    }

    pthread_mutex_lock(&srv->prompt_lock);
    if (++srv->prompt_next_id == 0)
        srv->prompt_next_id = 1;     /* 0 means "nothing" */
    const uint16_t id = srv->prompt_next_id;
    srv->prompt_id = id;
    srv->prompt_state = 0;
    srv->prompt_choice = 0;
    srv->prompt_answer[0] = '\0';
    pthread_mutex_unlock(&srv->prompt_lock);

    BsPrompt hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.id = id;
    hdr.kind = (uint8_t)kind;
    hdr.choices = (uint8_t)sent_choices;
    hdr.max_len = (uint16_t)(max_len > 0 ? max_len : 255);
    hdr.multiline = (uint8_t)(multiline ? 1 : 0);

    pthread_mutex_lock(&srv->roster);
    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->ready)
            client_send(cl, BS_MSG_PROMPT, 0, &hdr, sizeof(hdr), body, at);
    }
    pthread_mutex_unlock(&srv->roster);

    if (!srv->cfg.quiet)
        fprintf(stderr, "bottom_screen: asking the clients: %s\n", title);
    return id;
}

int bs_server_prompt_poll(BsServer *srv, uint16_t id,
                          char *text, size_t textlen, int *choice)
{
    if (!srv || id == 0)
        return -1;
    int state;
    pthread_mutex_lock(&srv->prompt_lock);
    if (srv->prompt_id != id) {
        state = -1;             /* withdrawn, or another question since */
    } else {
        state = srv->prompt_state;
        if (state == 1) {
            if (text && textlen) {
                size_t n = strlen(srv->prompt_answer);
                if (n >= textlen) n = textlen - 1;
                memcpy(text, srv->prompt_answer, n);
                text[n] = '\0';
            }
            if (choice)
                *choice = srv->prompt_choice;
        }
    }
    pthread_mutex_unlock(&srv->prompt_lock);
    return state;
}

void bs_server_prompt_cancel(BsServer *srv, uint16_t id)
{
    if (!srv || id == 0)
        return;
    pthread_mutex_lock(&srv->prompt_lock);
    const int mine = (srv->prompt_id == id);
    if (mine)
        srv->prompt_id = 0;
    pthread_mutex_unlock(&srv->prompt_lock);
    if (!mine)
        return;

    /* Told, not left on screen: a question the game has stopped waiting
     * for is a box somebody is still typing into. */
    BsPrompt hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.id = 0;
    pthread_mutex_lock(&srv->roster);
    for (int i = 0; i < srv->max_clients; i++) {
        BsClient *cl = &srv->clients[i];
        if (cl->in_use && !cl->gone && cl->ready)
            client_send(cl, BS_MSG_PROMPT, 0, &hdr, sizeof(hdr), NULL, 0);
    }
    pthread_mutex_unlock(&srv->roster);
}

uint16_t bs_server_port(const BsServer *srv) { return srv ? srv->port : 0; }
int bs_server_audio_source(const BsServer *srv)
{
    return srv ? srv->audio_source : BS_AUDIO_BOTH;
}

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
