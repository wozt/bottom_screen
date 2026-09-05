#ifndef BOTTOM_SCREEN_SERVER_H
#define BOTTOM_SCREEN_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include "bs_source.h"

/*
 * The server, as a library.
 *
 * It started life inside a main(), which was fine while the only source
 * was a test pattern. An emulator cannot be reorganised around someone
 * else's main, so the whole thing lives here and runs on its own thread:
 * melonDS creates one of these, hands it a source, and forgets about it.
 * The standalone bottom_screen_server binary is now a thin main over the
 * same code, which means the emulator build and the standalone build
 * cannot drift apart.
 *
 * The server never owns the source. The caller creates it, and destroys
 * it after the server is gone.
 */

typedef struct BsServer BsServer;

typedef struct {
    /* The port to try first. If it is taken the server walks upwards
     * until it finds a free one, so several emulators can run at once
     * without being told about each other. bs_server_port reports which
     * one it settled on. */
    uint16_t    port;       /* 0 = BS_DEFAULT_PORT */
    int         bitrate;    /* 0 = derived from the resolution */
    int         gop;        /* 0 = one keyframe per second */
    const char *encoder;    /* NULL = libx264 */
    int         quiet;      /* 1 = no progress on stdout */
} BsServerConfig;

/* Creates the server and starts its thread. */
BsServer *bs_server_create(BsSource *source, const BsServerConfig *cfg,
                           char *err, size_t errlen);

/* Stops the thread and waits for it. Safe to call more than once. */
void bs_server_stop(BsServer *srv);

/* Stops if still running, then frees. */
void bs_server_destroy(BsServer *srv);

/* The port the server actually bound, which may not be the one asked
 * for. 0 before it starts. */
uint16_t bs_server_port(const BsServer *srv);

/* 1 while a client is connected. */
int bs_server_has_client(const BsServer *srv);

/* Frames sent since the server started. */
uint32_t bs_server_frames(const BsServer *srv);

#endif /* BOTTOM_SCREEN_SERVER_H */
