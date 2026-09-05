#include "bs_decoder.h"
#include "bs_net.h"
#include "bs_protocol.h"

#include <SDL2/SDL.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * bottom_screen_client -- the Linux test client for Phase 1.
 *
 * Single-threaded on purpose: it polls the socket with a short timeout
 * and draws as soon as a frame lands, rather than handing frames between
 * a network thread and a render thread. At one client and these
 * resolutions there is nothing to gain from the extra thread, and a
 * queue between them would be somewhere for latency to hide.
 *
 * This client is a measuring instrument, not the product. The real
 * clients are the Android app and the Switch homebrew, which feed the
 * same bytes to their hardware decoders.
 */

typedef struct {
    int scale;          /* 0 = pick the largest integer that fits */
    SDL_Rect dst;       /* where the picture goes inside the window */
} View;

/*
 * Fill the window as far as the picture will go, centred, letterboxed.
 *
 * The rule is aspect ratio, not pixel grid: a DS screen is 4:3 and stays
 * 4:3, but it is scaled by whatever fraction fits rather than by whole
 * numbers only. Whole-number zoom keeps every source pixel exactly
 * square, which is lovely, but on a phone held in the hand it throws
 * away a third of the screen for that -- and the screen is 256 pixels
 * wide to begin with.
 *
 * What is never allowed is stretching to fill both axes independently,
 * which would make the picture fat or tall. That is the deformation
 * worth refusing; a fractional zoom is not.
 */
static void compute_view(View *v, int win_w, int win_h, int src_w, int src_h)
{
    if (v->scale > 0) {
        /* An explicit --scale still means exactly that multiple. */
        v->dst.w = src_w * v->scale;
        v->dst.h = src_h * v->scale;
    } else {
        /* Fit: the smaller of the two ratios, so neither axis overflows.
         * Computed in integers to avoid a rounding that would leave a
         * one-pixel sliver of background on one side. */
        int w = win_w;
        int h = (int)((long long)win_w * src_h / src_w);
        if (h > win_h) {
            h = win_h;
            w = (int)((long long)win_h * src_w / src_h);
        }
        v->dst.w = w;
        v->dst.h = h;
    }
    v->dst.x = (win_w - v->dst.w) / 2;
    v->dst.y = (win_h - v->dst.h) / 2;
}

/* Window pixel -> console pixel. Returns 0 if the point is outside the
 * picture, so a click on the letterbox is not reported as a touch at the
 * edge of the screen. */
static int window_to_console(const View *v, int wx, int wy, int src_w, int src_h,
                             int *cx, int *cy)
{
    if (wx < v->dst.x || wy < v->dst.y ||
        wx >= v->dst.x + v->dst.w || wy >= v->dst.y + v->dst.h)
        return 0;
    *cx = (wx - v->dst.x) * src_w / v->dst.w;
    *cy = (wy - v->dst.y) * src_h / v->dst.h;
    if (*cx < 0)      *cx = 0;
    if (*cx >= src_w) *cx = src_w - 1;
    if (*cy < 0)      *cy = 0;
    if (*cy >= src_h) *cy = src_h - 1;
    return 1;
}

static BsButton key_to_button(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP:     return BS_BTN_UP;
    case SDLK_DOWN:   return BS_BTN_DOWN;
    case SDLK_LEFT:   return BS_BTN_LEFT;
    case SDLK_RIGHT:  return BS_BTN_RIGHT;
    case SDLK_x:      return BS_BTN_A;
    case SDLK_z:      return BS_BTN_B;
    case SDLK_s:      return BS_BTN_X;
    case SDLK_a:      return BS_BTN_Y;
    case SDLK_q:      return BS_BTN_L;
    case SDLK_w:      return BS_BTN_R;
    case SDLK_e:      return BS_BTN_ZL;
    case SDLK_r:      return BS_BTN_ZR;
    case SDLK_RETURN: return BS_BTN_START;
    case SDLK_BACKSPACE: return BS_BTN_SELECT;
    default: return 0;
    }
}

static uint32_t g_seq = 0;

static void send_input(BsConn *conn, BsInputType type, int code, int x, int y)
{
    BsInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.sequence     = g_seq++;
    ev.timestamp_us = bs_now_us();
    ev.type         = (uint8_t)type;
    ev.code         = (uint8_t)code;
    ev.x            = (int16_t)x;
    ev.y            = (int16_t)y;
    bs_send_msg(conn, BS_MSG_INPUT, &ev, sizeof(ev), NULL, 0);
}

static const char *console_name(uint8_t c)
{
    switch (c) {
    case BS_CONSOLE_DS:   return "Nintendo DS";
    case BS_CONSOLE_3DS:  return "Nintendo 3DS";
    case BS_CONSOLE_WIIU: return "Wii U GamePad";
    }
    return "?";
}

static void usage(void)
{
    printf(
"bottom_screen_client -- shows a streamed bottom screen\n"
"\n"
"  --host NAME       server address (default 127.0.0.1)\n"
"  --port N          server port (default %d)\n"
"  --scale N         exact zoom factor; 0 fills the window (default 0)\n"
"  --help\n"
"\n"
"Mouse drags the touch screen. Keys: arrows d-pad, X/Z A/B, S/A X/Y,\n"
"Q/W shoulders, E/R ZL/ZR, Enter START, Backspace SELECT, Esc quits.\n",
    BS_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    uint16_t port = BS_DEFAULT_PORT;
    int scale = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--host") && next)  { host = next; i++; }
        else if (!strcmp(a, "--port") && next)  { port = (uint16_t)atoi(next); i++; }
        else if (!strcmp(a, "--scale") && next) { scale = atoi(next); i++; }
        else { fprintf(stderr, "unknown argument: %s\n", a); usage(); return 1; }
    }

    char err[256] = "";
    BsConn *conn = bs_connect(host, port, err, sizeof(err));
    if (!conn) { fprintf(stderr, "%s\n", err); return 1; }

    BsHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = BS_MAGIC;
    hello.version = BS_VERSION;
    if (bs_write_all(conn, &hello, sizeof(hello)) != 0) {
        fprintf(stderr, "handshake write failed\n");
        return 1;
    }

    BsHelloAck ack;
    if (bs_read_exact(conn, &ack, sizeof(ack)) != 0 ||
        ack.magic != BS_MAGIC || !ack.accepted) {
        fprintf(stderr, "server refused the connection\n");
        return 1;
    }
    if (ack.extradata_size) {
        uint8_t skip[4096];
        if (ack.extradata_size > sizeof(skip) ||
            bs_read_exact(conn, skip, ack.extradata_size) != 0) {
            fprintf(stderr, "bad extradata\n");
            return 1;
        }
    }

    const int src_w = ack.width, src_h = ack.height;
    printf("%s  %dx%d @ %u fps\n", console_name(ack.console), src_w, src_h,
           (unsigned)ack.fps);

    BsDecoder *dec = bs_decoder_create(err, sizeof(err));
    if (!dec) { fprintf(stderr, "%s\n", err); return 1; }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /*
     * Linear, because the zoom is now fractional. Nearest-neighbour at a
     * non-integer scale gives some source pixels two screen rows and
     * their neighbours one, which reads as a shimmering, uneven grid --
     * worse than a slight softness. At an exact whole-number --scale the
     * two look the same anyway.
     */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    int init_k = scale > 0 ? scale : 3;  /* the window's first size only */
    SDL_Window *win = SDL_CreateWindow("bottom_screen_client",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        src_w * init_k, src_h * init_k, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, src_w, src_h);
    if (!win || !ren || !tex) {
        fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return 1;
    }

    View view = { .scale = scale };
    uint8_t *buf = malloc(BS_MAX_PAYLOAD);
    if (!buf) { fprintf(stderr, "out of memory\n"); return 1; }

    int running = 1, dragging = 0, have_picture = 0;
    uint32_t frames = 0, last_report = bs_now_us();
    uint64_t latency_sum = 0; uint32_t latency_n = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int cx, cy;
            switch (e.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE) { running = 0; break; }
                if (e.key.repeat) break;
                { BsButton b = key_to_button(e.key.keysym.sym);
                  if (b) send_input(conn, BS_INPUT_BUTTON_DOWN, b, 0, 0); }
                break;
            case SDL_KEYUP:
                { BsButton b = key_to_button(e.key.keysym.sym);
                  if (b) send_input(conn, BS_INPUT_BUTTON_UP, b, 0, 0); }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT &&
                    window_to_console(&view, e.button.x, e.button.y, src_w, src_h, &cx, &cy)) {
                    dragging = 1;
                    send_input(conn, BS_INPUT_TOUCH_DOWN, 0, cx, cy);
                }
                break;
            case SDL_MOUSEMOTION:
                if (dragging &&
                    window_to_console(&view, e.motion.x, e.motion.y, src_w, src_h, &cx, &cy))
                    send_input(conn, BS_INPUT_TOUCH_MOVE, 0, cx, cy);
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT && dragging) {
                    dragging = 0;
                    send_input(conn, BS_INPUT_TOUCH_UP, 0, 0, 0);
                }
                break;
            default:
                break;
            }
        }
        if (!running)
            break;

        struct pollfd pfd = { .fd = bs_conn_fd(conn), .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 4);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint8_t type = 0;
            size_t n = 0;
            if (bs_recv_msg(conn, &type, buf, BS_MAX_PAYLOAD, &n) != 0) {
                printf("server closed the connection\n");
                break;
            }
            if (type == BS_MSG_VIDEO && n > sizeof(BsVideoHeader)) {
                BsVideoHeader vh;
                memcpy(&vh, buf, sizeof(vh));

                BsDecodedFrame f;
                int got = bs_decoder_decode(dec, buf + sizeof(vh),
                                            n - sizeof(vh), &f);
                if (got == 1) {
                    SDL_UpdateYUVTexture(tex, NULL,
                        f.y, f.y_stride, f.u, f.u_stride, f.v, f.v_stride);
                    have_picture = 1;
                    frames++;
                    /* Only meaningful when both ends share a clock, i.e.
                     * the same machine. Across two machines this is the
                     * difference between two unrelated monotonic clocks
                     * and means nothing. */
                    latency_sum += (uint64_t)(bs_now_us() - vh.timestamp_us);
                    latency_n++;
                }
            }
        } else if (pr < 0) {
            break;
        }

        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        compute_view(&view, ww, wh, src_w, src_h);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (have_picture)
            SDL_RenderCopy(ren, tex, NULL, &view.dst);
        SDL_RenderPresent(ren);

        uint32_t now = bs_now_us();
        if (now - last_report >= 1000000u) {
            char title[160];
            double lat = latency_n ? (double)latency_sum / latency_n / 1000.0 : 0.0;
            snprintf(title, sizeof(title),
                     "bottom_screen_client  %dx%d  ->%dx%d  %u fps  %.1f ms",
                     src_w, src_h, view.dst.w, view.dst.h, frames, lat);
            SDL_SetWindowTitle(win, title);
            printf("%u fps, same-machine latency %.1f ms\n", frames, lat);
            fflush(stdout);
            frames = 0; latency_sum = 0; latency_n = 0;
            last_report = now;
        }
    }

    free(buf);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    bs_decoder_destroy(dec);
    bs_conn_close(conn);
    return 0;
}
