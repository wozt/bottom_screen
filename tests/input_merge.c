/*
 * Two clients pressing the same button.
 *
 * With one client this was trivial: an event arrived, the source was
 * told. With several it stops being trivial, and the failure is quiet --
 * two people play, one lets go of A, and the other's A dies with it. Or
 * worse, somebody disconnects mid-jump and A stays held forever, which
 * looks exactly like the emulator hanging.
 *
 * None of that shows up on screen while you are testing alone, so it is
 * checked here instead.
 */
#include "bs_mailbox.h"
#include "bs_net.h"
#include "bs_protocol.h"
#include "bs_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void nap_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static BsConn *join(uint16_t port)
{
    char err[128];
    BsConn *c = bs_connect("127.0.0.1", port, err, sizeof(err));
    if (!c) { fprintf(stderr, "connect: %s\n", err); return NULL; }

    BsHello hello;
    memset(&hello, 0, sizeof(hello));
    hello.magic = BS_MAGIC;
    hello.version = BS_VERSION;
    if (bs_write_all(c, &hello, sizeof(hello)) != 0) { bs_conn_close(c); return NULL; }

    BsHelloAck ack;
    if (bs_read_exact(c, &ack, sizeof(ack)) != 0 || !ack.accepted) {
        bs_conn_close(c);
        return NULL;
    }
    for (uint16_t left = ack.extradata_size; left; ) {
        uint8_t skip[512];
        uint16_t n = left > sizeof(skip) ? (uint16_t)sizeof(skip) : left;
        if (bs_read_exact(c, skip, n) != 0) break;
        left = (uint16_t)(left - n);
    }
    return c;
}

static void press(BsConn *c, BsButton b, int down)
{
    BsInputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? BS_INPUT_BUTTON_DOWN : BS_INPUT_BUTTON_UP;
    ev.code = (uint8_t)b;
    bs_send_msg(c, BS_MSG_INPUT, &ev, sizeof(ev), NULL, 0);
}

/* The event travels over a socket and through a thread, so the state is
 * polled rather than read once. */
static int held(BsSource *src, BsButton b, int want, int within_ms)
{
    uint32_t bit = 1u << ((int)b - 1);
    for (int i = 0; i < within_ms / 5; i++) {
        BsInputState st;
        bs_mailbox_input(src, &st);
        if (!!(st.buttons & bit) == !!want)
            return 1;
        nap_ms(5);
    }
    return 0;
}

static void check(const char *what, int ok)
{
    printf("%-52s %s\n", what, ok ? "ok" : "ECHEC");
    if (!ok) failures++;
}

int main(void)
{
    BsSource *src = bs_mailbox_create(BS_CONSOLE_DS, 256, 192, 60,
                                      BS_PIXFMT_BGRA, 0, 0);
    if (!src) { fprintf(stderr, "no source\n"); return 1; }

    char err[256];
    BsServerConfig cfg = { .port = 5310, .quiet = 1 };
    BsServer *srv = bs_server_create(src, &cfg, err, sizeof(err));
    if (!srv) { fprintf(stderr, "no server: %s\n", err); return 1; }
    uint16_t port = bs_server_port(srv);

    BsConn *a = join(port);
    BsConn *b = join(port);
    if (!a || !b) { fprintf(stderr, "clients could not join\n"); return 1; }

    press(a, BS_BTN_A, 1);
    check("A held once one client presses it", held(src, BS_BTN_A, 1, 500));

    press(b, BS_BTN_A, 1);
    press(a, BS_BTN_A, 0);
    /* The other client is still holding it, so it must not come up. */
    nap_ms(150);
    check("A still held when only one of two releases",
          held(src, BS_BTN_A, 1, 100));

    press(b, BS_BTN_A, 0);
    check("A released once both let go", held(src, BS_BTN_A, 0, 500));

    /* Different buttons from different clients do not erase each other. */
    press(a, BS_BTN_X, 1);
    press(b, BS_BTN_Y, 1);
    check("X and Y held together from two clients",
          held(src, BS_BTN_X, 1, 500) && held(src, BS_BTN_Y, 1, 500));
    press(a, BS_BTN_X, 0);
    press(b, BS_BTN_Y, 0);

    /* Leaving mid-press must not leave the button down. */
    press(b, BS_BTN_B, 1);
    check("B held before the client vanishes", held(src, BS_BTN_B, 1, 500));
    bs_conn_close(b);
    check("B released when that client disconnects",
          held(src, BS_BTN_B, 0, 2000));

    bs_conn_close(a);
    bs_server_destroy(srv);
    src->destroy(src->self);

    printf(failures ? "\nECHEC (%d)\n" : "\nPASS\n", failures);
    return failures ? 1 : 0;
}
