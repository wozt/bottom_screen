#ifndef BOTTOM_SCREEN_WS_H
#define BOTTOM_SCREEN_WS_H

#include <stddef.h>
#include <stdint.h>

#include "bs_net.h"

/*
 * A browser on the same port as everything else.
 *
 * A native client opens with BsHello, whose first four bytes are the
 * magic "BSC1". A browser opens with "GET ". They cannot be confused, so
 * one listening socket serves both and there is no second port to
 * explain, forward or get wrong.
 *
 * Above the handshake the two are the same stream: one WebSocket binary
 * frame carries exactly one protocol message, header and payload, in the
 * same order and layout the TCP framing uses. The page and the phone are
 * reading the same bytes.
 */

/* True when the first bytes of a connection are an HTTP request rather
 * than a BsHello. Needs four bytes. */
int bs_ws_looks_like_http(const uint8_t *first, size_t len);

/*
 * Completes what the browser asked for, given the four bytes already
 * read.
 *
 * Returns 1 when a WebSocket is open and ready to carry messages, 0 when
 * the request was served and finished with (a page, or an error), and
 * -1 when the connection failed.
 */
int bs_ws_serve(BsConn *conn, char *err, size_t errlen);

/*
 * One binary frame carrying bytes that are not a protocol message: the
 * greeting, which is an ack and the parameter sets rather than anything
 * with a message header on it.
 */
int bs_ws_send_raw(BsConn *conn, const void *data, size_t len);

/* One message, as one binary frame. Same return convention as
 * bs_send_msg: 0 on success, negative on failure. */
int bs_ws_send_msg(BsConn *conn, uint8_t type,
                   const void *head, size_t head_len,
                   const void *body, size_t body_len);

/*
 * Reads one message out of one frame. Returns 0 with *out_len set, 1 on
 * a clean close, -1 on error -- the same convention as bs_recv_msg, for
 * the same reason: a zero-length message and an ended connection have to
 * stay distinguishable.
 */
int bs_ws_recv_msg(BsConn *conn, uint8_t *type, void *buf, size_t bufcap,
                   size_t *out_len);

#endif /* BOTTOM_SCREEN_WS_H */
