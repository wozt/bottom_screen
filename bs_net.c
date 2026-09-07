#include "bs_net.h"
#include "bs_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifndef __SWITCH__
/* The Switch has struct iovec and sendmsg, but declares them in
 * sys/socket.h rather than in a sys/uio.h it does not ship. */
#include <sys/uio.h>
#endif
#include <time.h>
#include <unistd.h>

struct BsConn {
    int  fd;
    char peer[64];
};

static void __attribute__((format(printf, 3, 4)))
set_err(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || errlen == 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

uint32_t bs_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u);
}

/*
 * Nagle batches small writes and waits for an ack before sending the
 * next partial segment. On a stream of small frames that is up to 40 ms
 * of delay added to every one of them -- more than two frames at 60 Hz,
 * and entirely invisible in a throughput test. Off, always.
 */
static void tune_socket(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static BsConn *conn_new(int fd, const char *peer)
{
    BsConn *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    snprintf(c->peer, sizeof(c->peer), "%s", peer ? peer : "?");
    tune_socket(fd);
    return c;
}

int bs_listen(uint16_t port, char *err, size_t errlen)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        set_err(err, errlen, "bind port %u: %s", (unsigned)port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        set_err(err, errlen, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

BsConn *bs_accept(int listen_fd, char *err, size_t errlen)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int fd = accept(listen_fd, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        set_err(err, errlen, "accept: %s", strerror(errno));
        return NULL;
    }

    char host[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, host, sizeof(host));
    char label[64];
    snprintf(label, sizeof(label), "%s:%u", host, (unsigned)ntohs(peer.sin_port));

    BsConn *c = conn_new(fd, label);
    if (!c)
        set_err(err, errlen, "out of memory");
    return c;
}

BsConn *bs_connect(const char *host, uint16_t port, char *err, size_t errlen)
{
    /*
     * An address typed as four numbers is already an address, so it goes
     * straight to connect rather than through a resolver.
     *
     * On a PC that only saves a lookup. On the Switch it is the
     * difference between working and not: getaddrinfo fails there, and
     * an IP address is what somebody types on a console anyway.
     */
    struct in_addr numeric;
    if (inet_pton(AF_INET, host, &numeric) == 1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            set_err(err, errlen, "socket: %s", strerror(errno));
            return NULL;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr = numeric;
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            set_err(err, errlen, "connect %s:%u: %s",
                    host, (unsigned)port, strerror(errno));
            close(fd);
            return NULL;
        }
        char numeric_label[64];
        snprintf(numeric_label, sizeof(numeric_label), "%s:%u",
                 host, (unsigned)port);
        BsConn *nc = conn_new(fd, numeric_label);
        if (!nc) {
            set_err(err, errlen, "out of memory");
            close(fd);
        }
        return nc;
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        set_err(err, errlen, "resolve %s: %s", host, gai_strerror(rc));
        return NULL;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        set_err(err, errlen, "connect %s:%u: %s", host, (unsigned)port, strerror(errno));
        return NULL;
    }

    char label[64];
    snprintf(label, sizeof(label), "%s:%u", host, (unsigned)port);
    BsConn *c = conn_new(fd, label);
    if (!c)
        set_err(err, errlen, "out of memory");
    return c;
}

void bs_conn_close(BsConn *conn)
{
    if (!conn)
        return;
    if (conn->fd >= 0)
        close(conn->fd);
    free(conn);
}

int bs_conn_fd(const BsConn *conn) { return conn ? conn->fd : -1; }
const char *bs_conn_peer(const BsConn *conn) { return conn ? conn->peer : "?"; }

int bs_write_all(BsConn *conn, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(conn->fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int bs_read_exact(BsConn *conn, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = recv(conn->fd, p, len, 0);
        if (n == 0)
            return 1;           /* clean close */
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int bs_send_msg(BsConn *conn, uint8_t type,
                const void *head, size_t head_len,
                const void *body, size_t body_len)
{
    if (!conn)
        return -1;

    BsMsgHeader h;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.payload_size = (uint32_t)(head_len + body_len);

    if (h.payload_size > BS_MAX_PAYLOAD)
        return -1;

    struct iovec iov[3];
    int n = 0;
    iov[n].iov_base = &h;             iov[n].iov_len = sizeof(h);         n++;
    if (head_len) { iov[n].iov_base = (void *)head; iov[n].iov_len = head_len; n++; }
    if (body_len) { iov[n].iov_base = (void *)body; iov[n].iov_len = body_len; n++; }

    size_t total = sizeof(h) + head_len + body_len;
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = (size_t)n;

    /* One sendmsg keeps the header and the frame in the same segment,
     * which matters with TCP_NODELAY on: two writes would be two small
     * packets on the wire. Partial sends fall back to a plain loop. */
    while (total > 0) {
        ssize_t sent = sendmsg(conn->fd, &msg, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total -= (size_t)sent;
        if (total == 0)
            break;

        /* Advance the iovec past what went out. */
        size_t skip = (size_t)sent;
        while (skip > 0 && msg.msg_iovlen > 0) {
            if (skip >= msg.msg_iov[0].iov_len) {
                skip -= msg.msg_iov[0].iov_len;
                msg.msg_iov++;
                msg.msg_iovlen--;
            } else {
                msg.msg_iov[0].iov_base = (uint8_t *)msg.msg_iov[0].iov_base + skip;
                msg.msg_iov[0].iov_len -= skip;
                skip = 0;
            }
        }
    }
    return 0;
}

int bs_recv_msg(BsConn *conn, uint8_t *type, void *buf, size_t bufcap,
                size_t *out_len)
{
    if (!conn)
        return -1;

    BsMsgHeader h;
    int rc = bs_read_exact(conn, &h, sizeof(h));
    if (rc != 0)
        return rc;

    if (h.payload_size > BS_MAX_PAYLOAD || h.payload_size > bufcap)
        return -1;

    if (h.payload_size > 0) {
        rc = bs_read_exact(conn, buf, h.payload_size);
        if (rc != 0)
            return rc;
    }

    if (type)
        *type = h.type;
    if (out_len)
        *out_len = h.payload_size;
    return 0;
}
