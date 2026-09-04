#ifndef BOTTOM_SCREEN_NET_H
#define BOTTOM_SCREEN_NET_H

#include <stddef.h>
#include <stdint.h>

/*
 * Blocking TCP transport with the framing from bs_protocol.h.
 *
 * TCP first, deliberately. It is the wrong transport for the finished
 * thing -- a lost packet must not stall the frames behind it -- but it
 * removes fragmentation, reordering and loss from the picture while the
 * protocol, the encoder and the decoder are all new at once. UDP lands
 * once this pipeline is known good, and the message header already
 * carries the fragmentation fields it will need.
 */

typedef struct BsConn BsConn;

/* Returns a listening socket, or -1. */
int  bs_listen(uint16_t port, char *err, size_t errlen);

/* Blocks until a client connects. */
BsConn *bs_accept(int listen_fd, char *err, size_t errlen);

BsConn *bs_connect(const char *host, uint16_t port, char *err, size_t errlen);

void bs_conn_close(BsConn *conn);
int  bs_conn_fd(const BsConn *conn);
const char *bs_conn_peer(const BsConn *conn);

/*
 * Sends one framed message. head and body are written with a single
 * writev, so an encoded frame is never copied just to be prefixed.
 * Either may be NULL/0. Returns 0, or -1 with the connection unusable.
 */
int bs_send_msg(BsConn *conn, uint8_t type,
                const void *head, size_t head_len,
                const void *body, size_t body_len);

/*
 * Reads one framed message into buf. Returns the payload length, 0 on a
 * clean peer close, -1 on error. A payload larger than bufcap or than
 * BS_MAX_PAYLOAD is an error rather than an allocation.
 */
long bs_recv_msg(BsConn *conn, uint8_t *type, void *buf, size_t bufcap);

/* Blocking read/write of an exact byte count, for the handshake, which
 * is not framed. */
int bs_read_exact(BsConn *conn, void *buf, size_t len);
int bs_write_all(BsConn *conn, const void *buf, size_t len);

/* CLOCK_MONOTONIC in microseconds, truncated to 32 bits to match the
 * protocol's timestamp fields. Wraps every ~71 minutes, which is fine
 * for a latency measurement and never used as an absolute time. */
uint32_t bs_now_us(void);

#endif /* BOTTOM_SCREEN_NET_H */
