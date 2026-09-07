#include "bs_ws.h"

#include "bs_protocol.h"
#include "web_page.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/* ---------------------------------------------------------------- sha1 */

/*
 * SHA-1 and base64 in full, rather than linking libcrypto.
 *
 * bs_server.c is compiled into three emulators, so every library it
 * touches has to be added to three build systems -- which is what adding
 * libswresample cost. Ninety lines of a fixed, well-known algorithm is
 * the cheaper side of that trade, and this is the only thing that needed
 * it.
 */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  block[64];
    size_t   used;
} Sha1;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);            k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len = 0;
    s->used = 0;
}

static void sha1_update(Sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len > 0) {
        size_t take = 64 - s->used;
        if (take > len) take = len;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64) {
            sha1_block(s, s->block);
            s->used = 0;
        }
    }
}

static void sha1_final(Sha1 *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0;
    while (s->used != 56)
        sha1_update(s, &zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; i++)
        tail[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, tail, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? T[v & 63] : '=';
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------- request */

int bs_ws_looks_like_http(const uint8_t *first, size_t len)
{
    /* A native client opens with the magic, whose bytes spell BSC1. No
     * HTTP method begins with that, and no method this serves is longer
     * than four characters plus a space. */
    return len >= 4 && memcmp(first, "GET ", 4) == 0;
}

/* Reads until the blank line that ends the request head, or gives up. */
static int read_request(BsConn *conn, char *out, size_t cap)
{
    size_t n = 0;
    while (n < cap - 1) {
        if (n >= 4 && memcmp(out + n - 4, "\r\n\r\n", 4) == 0)
            break;
        ssize_t got = recv(bs_conn_fd(conn), out + n, 1, 0);
        if (got <= 0)
            return -1;
        n += (size_t)got;
    }
    out[n] = '\0';
    return 0;
}

/* Case-insensitive header lookup; browsers do not agree on capitals. */
static const char *header_value(const char *request, const char *name)
{
    size_t namelen = strlen(name);
    for (const char *l = request; l && *l; ) {
        const char *eol = strstr(l, "\r\n");
        if (!eol)
            break;
        if ((size_t)(eol - l) > namelen && strncasecmp(l, name, namelen) == 0 &&
            l[namelen] == ':') {
            const char *v = l + namelen + 1;
            while (*v == ' ')
                v++;
            return v;
        }
        l = eol + 2;
    }
    return NULL;
}

static int send_all(BsConn *conn, const void *buf, size_t len)
{
    return bs_write_all(conn, buf, len);
}

static int serve_page(BsConn *conn)
{
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %u\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     (unsigned)BS_WEB_PAGE_LEN);
    if (send_all(conn, head, (size_t)n) != 0)
        return -1;
    return send_all(conn, BS_WEB_PAGE, BS_WEB_PAGE_LEN);
}

int bs_ws_serve(BsConn *conn, char *err, size_t errlen)
{
    char request[4096];
    if (read_request(conn, request, sizeof(request)) != 0) {
        snprintf(err, errlen, "truncated request");
        return -1;
    }

    const char *key = header_value(request, "Sec-WebSocket-Key");
    const char *upgrade = header_value(request, "Upgrade");
    if (!key || !upgrade || strncasecmp(upgrade, "websocket", 9) != 0) {
        /* An ordinary page request. Serving it and closing is the whole
         * of what a browser needs before it can open the socket. */
        serve_page(conn);
        return 0;
    }

    /* The fixed string is from RFC 6455: the client's key concatenated
     * with it, hashed and base64'd, is how the server proves it
     * understood rather than merely echoed. */
    char accept_src[128];
    size_t keylen = strcspn(key, "\r\n ");
    if (keylen > 64) {
        snprintf(err, errlen, "key too long");
        return -1;
    }
    memcpy(accept_src, key, keylen);
    memcpy(accept_src + keylen, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);

    Sha1 sha;
    uint8_t digest[20];
    sha1_init(&sha);
    sha1_update(&sha, accept_src, keylen + 36);
    sha1_final(&sha, digest);

    char accept[32];
    base64(digest, sizeof(digest), accept);

    char reply[256];
    int n = snprintf(reply, sizeof(reply),
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
    if (send_all(conn, reply, (size_t)n) != 0) {
        snprintf(err, errlen, "could not complete the handshake");
        return -1;
    }
    return 1;
}

/* -------------------------------------------------------------- frames */

/* The frame header for a binary payload of this size. */
static size_t frame_header(uint8_t *frame, size_t payload)
{
    size_t flen = 0;
    frame[flen++] = 0x82;            /* FIN, binary */
    if (payload < 126) {
        frame[flen++] = (uint8_t)payload;
    } else if (payload <= 0xFFFF) {
        frame[flen++] = 126;
        frame[flen++] = (uint8_t)(payload >> 8);
        frame[flen++] = (uint8_t)payload;
    } else {
        frame[flen++] = 127;
        for (int i = 7; i >= 0; i--)
            frame[flen++] = (uint8_t)((uint64_t)payload >> (i * 8));
    }
    return flen;
}

int bs_ws_send_raw(BsConn *conn, const void *data, size_t len)
{
    uint8_t frame[10];
    const size_t flen = frame_header(frame, len);
    if (send_all(conn, frame, flen) != 0)
        return -1;
    return send_all(conn, data, len);
}

int bs_ws_send_msg(BsConn *conn, uint8_t type,
                   const void *head, size_t head_len,
                   const void *body, size_t body_len)
{
    BsMsgHeader h;
    memset(&h, 0, sizeof(h));
    h.type = type;
    h.payload_size = (uint32_t)(head_len + body_len);

    const size_t payload = sizeof(h) + head_len + body_len;

    uint8_t frame[10];
    const size_t flen = frame_header(frame, payload);

    /* Four writes at worst, all small but the last: the socket has
     * TCP_NODELAY, so they are coalesced by the kernel's own buffer
     * rather than sent as four packets. */
    if (send_all(conn, frame, flen) != 0)
        return -1;
    if (send_all(conn, &h, sizeof(h)) != 0)
        return -1;
    if (head_len && send_all(conn, head, head_len) != 0)
        return -1;
    if (body_len && send_all(conn, body, body_len) != 0)
        return -1;
    return 0;
}

static int read_exact(BsConn *conn, void *buf, size_t len)
{
    return bs_read_exact(conn, buf, len);
}

int bs_ws_recv_msg(BsConn *conn, uint8_t *type, void *buf, size_t bufcap,
                   size_t *out_len)
{
    for (;;) {
        uint8_t hdr[2];
        if (read_exact(conn, hdr, 2) != 0)
            return 1;                     /* peer went away */

        const int opcode = hdr[0] & 0x0F;
        const int masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (read_exact(conn, ext, 2) != 0) return 1;
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (read_exact(conn, ext, 8) != 0) return 1;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | ext[i];
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && read_exact(conn, mask, 4) != 0)
            return 1;

        if (opcode == 0x8)                /* close */
            return 1;

        if (len > bufcap || len > BS_MAX_PAYLOAD + sizeof(BsMsgHeader))
            return -1;

        uint8_t *p = buf;
        if (len && read_exact(conn, p, (size_t)len) != 0)
            return 1;
        /* Everything a browser sends is masked; unmasking is a xor with
         * a four-byte key that repeats. */
        if (masked)
            for (uint64_t i = 0; i < len; i++)
                p[i] ^= mask[i & 3];

        if (opcode == 0x9) {              /* ping: answer and keep reading */
            uint8_t pong[2] = { 0x8A, (uint8_t)(len < 126 ? len : 0) };
            send_all(conn, pong, 2);
            if (len < 126 && len)
                send_all(conn, p, (size_t)len);
            continue;
        }
        if (opcode != 0x2)                /* only binary carries messages */
            continue;

        if (len < sizeof(BsMsgHeader))
            return -1;

        BsMsgHeader h;
        memcpy(&h, p, sizeof(h));
        if (h.payload_size != len - sizeof(BsMsgHeader))
            return -1;

        *type = h.type;
        *out_len = (size_t)(len - sizeof(BsMsgHeader));
        memmove(buf, p + sizeof(BsMsgHeader), *out_len);
        return 0;
    }
}
