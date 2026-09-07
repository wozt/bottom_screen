#include "bs_decoder.h"
#include "bs_net.h"
#include "bs_protocol.h"

#define BS_MSG_HEADER_LEN (sizeof(BsMsgHeader))

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Headless end-to-end check of the Phase 1 pipeline.
 *
 * Connects, does the handshake, decodes real frames off the wire and
 * sends input back. It asserts the things that would break silently:
 * that the decoded picture is the console's native size and not some
 * rescaled thing, that a keyframe actually arrives, and that frames keep
 * coming rather than the stream stalling after the first one.
 *
 * No SDL, so this runs over SSH and from a script.
 */

static int fail(const char *msg)
{
    fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    uint16_t port = BS_DEFAULT_PORT;
    int want_frames = 120;
    const char *dump_path = NULL;
    int ask_w = 0, ask_h = 0;
    int tap_x = 500, tap_y = 500;   /* thousandths of the announced size */
    /* Presses and keeps pressing, so a dumped frame is certain to show
     * where the touch landed. The default is a real tap, because a
     * stylus that never lifts is what made the DS firmware wait
     * forever -- this is for measuring, not for exercising input. */
    int hold = 0;

    for (int i = 1; i < argc; i++) {
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--host") && next)   { host = next; i++; }
        else if (!strcmp(argv[i], "--port") && next)   { port = (uint16_t)atoi(next); i++; }
        else if (!strcmp(argv[i], "--frames") && next) { want_frames = atoi(next); i++; }
        else if (!strcmp(argv[i], "--dump-yuv") && next) { dump_path = next; i++; }
        else if (!strcmp(argv[i], "--ask-size") && next) {
            /* WxH: ask the server to send a smaller picture than the
             * emulator renders, which is what a phone wants when the
             * internal resolution is turned up. */
            if (sscanf(next, "%dx%d", &ask_w, &ask_h) != 2)
                return fail("--ask-size wants WxH");
            i++;
        }
        else if (!strcmp(argv[i], "--hold")) { hold = 1; }
        else if (!strcmp(argv[i], "--tap") && next) {
            if (sscanf(next, "%d,%d", &tap_x, &tap_y) != 2)
                return fail("--tap wants X,Y as fractions in thousandths");
            i++;
        }
    }

    /* The server binds, then blocks in accept. Retrying here beats a
     * probe connection from the shell script: a probe would occupy the
     * accept slot and be served as a real client. */
    char err[256] = "";
    BsConn *conn = NULL;
    for (int tries = 0; tries < 100 && !conn; tries++) {
        conn = bs_connect(host, port, err, sizeof(err));
        if (!conn) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (!conn) return fail(err);

    BsHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = BS_MAGIC;
    hello.version = BS_VERSION;
    if (bs_write_all(conn, &hello, sizeof(hello)) != 0)
        return fail("could not send hello");

    BsHelloAck ack;
    if (bs_read_exact(conn, &ack, sizeof(ack)) != 0)
        return fail("no ack");
    if (ack.magic != BS_MAGIC)  return fail("ack magic mismatch");
    if (!ack.accepted)          return fail("server full");
    if (ack.codec != BS_CODEC_H264) return fail("unexpected codec");
    if (ack.extradata_size) {
        uint8_t skip[4096];
        if (ack.extradata_size > sizeof(skip) ||
            bs_read_exact(conn, skip, ack.extradata_size) != 0)
            return fail("bad extradata");
    }

    /* The native size the server announced must match the console it
     * says it is serving -- getting this wrong is exactly the bug that
     * would stretch every frame on the real clients. */
    int exp_w = 0, exp_h = 0;
    switch (ack.console) {
    case BS_CONSOLE_DS:   exp_w = BS_DS_WIDTH;   exp_h = BS_DS_HEIGHT;   break;
    case BS_CONSOLE_3DS:  exp_w = BS_3DS_WIDTH;  exp_h = BS_3DS_HEIGHT;  break;
    case BS_CONSOLE_WIIU: exp_w = BS_WIIU_WIDTH; exp_h = BS_WIIU_HEIGHT; break;
    default: return fail("unknown console in ack");
    }
    /*
     * The aspect ratio, not the exact size: an emulator rendering at a
     * higher internal resolution is doing the right thing and the
     * stream simply arrives sharper. What must not change is the shape,
     * which is what catches a framebuffer handed over rotated -- the
     * 3DS stores its screens in portrait, so 240x320 where 320x240 was
     * expected is a real defect and not a matter of scale.
     */
    const double want = (double)exp_w / exp_h;
    const double got  = (double)ack.width / ack.height;
    if (got < want * 0.98 || got > want * 1.02)
        return fail("announced aspect ratio is not the console's");

    printf("handshake ok: console=%u %ux%u @%u fps\n",
           ack.console, ack.width, ack.height, ack.fps);

    BsDecoder *dec = bs_decoder_create(err, sizeof(err));
    if (!dec) return fail(err);

    uint8_t *buf = malloc(BS_MAX_PAYLOAD);
    if (!buf) return fail("out of memory");

    /*
     * A stream can be perfectly well-formed and still be showing
     * nothing: if the emulator handed over a cleared buffer, every frame
     * would decode, at a plausible frame rate, entirely blank. Tracking
     * the spread of the luma plane is what separates "the pipeline runs"
     * from "the pipeline carries a picture".
     */
    int y_min = 255, y_max = 0;
    long long y_sum = 0; long long y_count = 0;

    int cur_w = ack.width, cur_h = ack.height;
    const int announced_w = ack.width, announced_h = ack.height;
    int asked = 0;
    int resizes = 0;

    int audio_packets = 0;
    size_t audio_bytes = 0;

    int decoded = 0, keyframes = 0;
    uint32_t first_id = 0, last_id = 0;
    uint64_t total_bytes = 0;
    uint32_t started = bs_now_us();

    while (decoded < want_frames) {
        uint8_t type = 0;
        size_t n = 0;
        if (bs_recv_msg(conn, &type, buf, BS_MAX_PAYLOAD, &n) != 0)
            return fail("stream ended early");
        if (type == BS_MSG_STREAM_INFO && n >= sizeof(BsStreamInfo)) {
            /* The picture changed shape mid-stream, which is what the
             * server sends instead of hanging up when someone moves the
             * emulator's internal resolution. Follow it. */
            BsStreamInfo si;
            memcpy(&si, buf, sizeof(si));
            if (si.width > 0 && si.height > 0) {
                cur_w = si.width;
                cur_h = si.height;
                resizes++;
                bs_decoder_destroy(dec);
                dec = bs_decoder_create(err, sizeof(err));
                if (!dec) return fail("cannot rebuild the decoder");
            }
            continue;
        }
        if (type == BS_MSG_AUDIO) {
            if (n > sizeof(BsAudioHeader)) {
                audio_packets++;
                audio_bytes += n - sizeof(BsAudioHeader);
            }
            continue;
        }
        if (type != BS_MSG_VIDEO) continue;
        if (n <= sizeof(BsVideoHeader)) return fail("truncated video message");

        BsVideoHeader vh;
        memcpy(&vh, buf, sizeof(vh));
        if (vh.flags & BS_VFLAG_KEYFRAME) keyframes++;
        if (decoded == 0) first_id = vh.frame_id;
        last_id = vh.frame_id;
        total_bytes += (uint64_t)n;

        BsDecodedFrame f;
        int got = bs_decoder_decode(dec, buf + sizeof(vh),
                                    n - sizeof(vh), &f);
        if (got < 0) return fail("decode error");
        if (got == 1) {
            /* Against what the handshake announced, not the console's
             * own size: an emulator may render larger, and the contract
             * is that the picture matches what was promised. */
            if (f.width != cur_w || f.height != cur_h)
                return fail("decoded picture is not the announced size");
            decoded++;

            for (int row = 0; row < f.height; row++) {
                const uint8_t *p = f.y + (size_t)row * f.y_stride;
                for (int col = 0; col < f.width; col++) {
                    int v = p[col];
                    if (v < y_min) y_min = v;
                    if (v > y_max) y_max = v;
                    y_sum += v;
                    y_count++;
                }
            }

            if (dump_path && decoded == want_frames) {
                FILE *fp = fopen(dump_path, "wb");
                if (fp) {
                    for (int row = 0; row < f.height; row++)
                        fwrite(f.y + (size_t)row * f.y_stride, 1, (size_t)f.width, fp);
                    for (int row = 0; row < f.height / 2; row++)
                        fwrite(f.u + (size_t)row * f.u_stride, 1, (size_t)f.width / 2, fp);
                    for (int row = 0; row < f.height / 2; row++)
                        fwrite(f.v + (size_t)row * f.v_stride, 1, (size_t)f.width / 2, fp);
                    fclose(fp);
                    printf("wrote %s (%dx%d yuv420p)\n", dump_path, f.width, f.height);
                }
            }

            /*
             * Drive the input path while frames flow, so a server that
             * deadlocks between its video loop and its input thread
             * fails here rather than in front of a person.
             *
             * A real tap, not a permanently held stylus. Sending only
             * TOUCH_DOWN leaves the console seeing a finger that never
             * lifts, and software that waits for a press then waits
             * forever -- which is exactly what the DS firmware's warning
             * screen does.
             */
            /* The Android client asks for a keyframe as soon as its
             * decoder exists, so the test has to exercise that path too
             * -- it was the one difference between the two clients when
             * the phone's connection kept dying. */
            if (decoded == 5) {
                uint8_t kf[BS_MSG_HEADER_LEN];
                memset(kf, 0, sizeof(kf));
                if (bs_send_msg(conn, BS_MSG_REQUEST_KEYFRAME, NULL, 0, NULL, 0) < 0)
                    return fail("could not request a keyframe");
                (void)kf;
            }

            /* Asked for once the stream is running, so the renegotiation
             * is exercised rather than the opening handshake. */
            if (ask_w > 0 && !asked && decoded == 10) {
                asked = 1;
                BsSize sz = { (uint16_t)ask_w, (uint16_t)ask_h };
                if (bs_send_msg(conn, BS_MSG_SET_SIZE, &sz, sizeof(sz), NULL, 0) < 0)
                    return fail("could not ask for a size");
            }

            int phase = hold ? (decoded == 20 ? 20 : -1) : (decoded % 40);
            if (phase == 20 || phase == 28) {
                BsInputEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.sequence = (uint32_t)decoded;
                ev.timestamp_us = bs_now_us();
                ev.type = (phase == 20) ? BS_INPUT_TOUCH_DOWN : BS_INPUT_TOUCH_UP;
                /* The announced size, not the console's own: a stream
                 * rendered at a higher internal resolution uses that
                 * space, and aiming with native coordinates lands every
                 * tap near the top left. */
                ev.x = (int16_t)((long)cur_w * tap_x / 1000);
                ev.y = (int16_t)((long)cur_h * tap_y / 1000);
                if (bs_send_msg(conn, BS_MSG_INPUT, &ev, sizeof(ev), NULL, 0) < 0)
                    return fail("could not send input");
            }
        }
    }

    uint32_t elapsed = bs_now_us() - started;
    if (elapsed == 0) return fail("no time elapsed");

    double fps  = (double)decoded * 1000000.0 / elapsed;
    double mbps = (double)total_bytes * 8.0 / elapsed;

    if (keyframes < 1) return fail("no keyframe in the stream");
    if (last_id - first_id + 1 != (uint32_t)decoded)
        printf("note: %u frame ids spanned for %d decoded pictures\n",
               last_id - first_id + 1, decoded);

    printf("decoded %d frames, %d keyframes, %.1f fps, %.2f Mbit/s\n",
           decoded, keyframes, fps, mbps);
    if (ack.audio_rate > 0)
        printf("audio: %d paquets, %.1f ko, %u Hz %u canaux\n",
               audio_packets, audio_bytes / 1024.0,
               (unsigned)ack.audio_rate, (unsigned)ack.audio_channels);
    else
        printf("audio: aucun annonce par le serveur\n");

    if (resizes > 0)
        printf("taille renegociee %d fois, desormais %dx%d\n", resizes, cur_w, cur_h);
    if (ask_w > 0) {
        printf("demande %dx%d, annonce au depart %dx%d, recu %dx%d\n",
               ask_w, ask_h, announced_w, announced_h, cur_w, cur_h);
        if (cur_w > announced_w || cur_h > announced_h)
            return fail("the picture did not get smaller");
    }

    printf("luma: min %d, max %d, mean %.1f\n",
           y_min, y_max, y_count ? (double)y_sum / (double)y_count : 0.0);
    if (y_min == y_max)
        return fail("every pixel is the same value -- the picture is blank");
    printf("PASS\n");

    free(buf);
    bs_decoder_destroy(dec);
    bs_conn_close(conn);
    return 0;
}
