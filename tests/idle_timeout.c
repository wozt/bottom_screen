/*
 * The idle timeout, and the thing it must never do.
 *
 * A client that waits for ever cannot tell a stream that stopped from a
 * stream that is merely quiet, so a GamePad watching a screen whose
 * server had gone to sleep stayed frozen with its socket open and empty.
 * The timeout is what lets the loop notice.
 *
 * The hazard is the fix, not the fault: a timeout that fires part way
 * through a message leaves the stream one header out of step and every
 * message after it is garbage. So it is armed before a message begins
 * and cleared once it has, and that is what is checked here -- a sender
 * that dribbles a message out in pieces, slower than the timeout, must
 * still be read whole.
 */
#include "../bs_net.h"
#include "../bs_protocol.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static uint16_t port;

static void nap(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

/* Sends one message a byte at a time, far slower than the timeout. */
static void *dribbler(void *unused)
{
    (void)unused;
    char err[128] = "";
    BsConn *c = bs_connect("127.0.0.1", port, err, sizeof(err));
    assert(c);

    nap(400);               /* silence first: the reader must report it */

    uint8_t body[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t msg[sizeof(BsMsgHeader) + sizeof(body)];
    BsMsgHeader h;
    memset(&h, 0, sizeof(h));
    h.type = BS_MSG_PING;
    h.payload_size = sizeof(body);
    memcpy(msg, &h, sizeof(h));
    memcpy(msg + sizeof(h), body, sizeof(body));

    for (size_t i = 0; i < sizeof(msg); i++) {
        bs_write_all(c, msg + i, 1);
        nap(30);            /* 11 bytes at 30ms is well past 100ms */
    }
    nap(300);
    bs_conn_close(c);
    return NULL;
}

int main(void)
{
    char err[128] = "";
    /* Port 0 lets the kernel choose; ask the socket which it chose. */
    int l = bs_listen(0, err, sizeof(err));
    assert(l >= 0);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    assert(getsockname(l, (struct sockaddr *)&addr, &len) == 0);
    port = ntohs(addr.sin_port);

    pthread_t t;
    pthread_create(&t, NULL, dribbler, NULL);

    BsConn *s = bs_accept(l, err, sizeof(err));
    assert(s);
    bs_conn_set_idle_timeout(s, 100);

    uint8_t type = 0, buf[64];
    size_t n = 0;

    /* Silence is reported as a timeout, not as a dead connection. */
    int quiet = 0;
    while (bs_recv_msg(s, &type, buf, sizeof(buf), &n) != 0) {
        assert(bs_conn_timed_out(s));
        if (++quiet > 20) { puts("FAIL: the message never arrived"); return 1; }
    }
    assert(quiet >= 1);

    /* And the message that was dribbled out arrived whole and in order. */
    assert(!bs_conn_timed_out(s));
    assert(type == BS_MSG_PING);
    assert(n == 8);
    for (int i = 0; i < 8; i++)
        assert(buf[i] == i + 1);

    pthread_join(t, NULL);
    bs_conn_close(s);
    close(l);
    printf("PASS: silence reported after %d timeouts, message read whole\n", quiet);
    return 0;
}
