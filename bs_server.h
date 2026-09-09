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
    /* Which card a hardware encoder should use. NULL takes whatever the
     * driver offers first, which is right with one GPU and a guess with
     * two -- only one of them may have an encoder. */
    const char *device;
    int         quiet;      /* 1 = no progress on stdout */

    /* How many clients may watch at once. 0 = 4. They share one
     * encoder, so a second viewer costs bandwidth rather than a core --
     * and shares the quality setting with everyone else. */
    int         max_clients;
} BsServerConfig;

/* Creates the server and starts its thread. */
BsServer *bs_server_create(BsSource *source, const BsServerConfig *cfg,
                           char *err, size_t errlen);

/* Stops the thread and waits for it. Safe to call more than once. */
void bs_server_stop(BsServer *srv);

/* Stops if still running, then frees. */
void bs_server_destroy(BsServer *srv);

/*
 * Offers the machine's other screen -- the top one on a DS or a 3DS, the
 * television picture on a Wii U.
 *
 * Optional, and separate from bs_server_create because it is: a backend
 * that only has the bottom screen to give simply never calls this, and
 * its clients are told there is no choice to make rather than being
 * offered one that does nothing.
 *
 * The server does not own this source any more than it owns the other
 * one. Pass NULL to withdraw it; anyone watching it is moved back.
 */
void bs_server_set_top_source(BsServer *srv, BsSource *top);

/*
 * Says there is a top screen, before there is one to hand over.
 *
 * For a backend that only produces the picture while somebody is
 * watching it -- which is all three of them, because reading a texture
 * back off the GPU every frame is not free. Without this the two facts
 * deadlock: no client may choose a screen the server does not admit to,
 * and the backend will not produce one until a client chooses it.
 *
 * Call it once the server is up; call bs_server_set_top_source with the
 * real thing on the first frame after somebody asks.
 */
void bs_server_offer_top(BsServer *srv);

/*
 * Whether anybody is watching the given BsScreen right now.
 *
 * For the backends, which is where the real cost of a second screen
 * lands: reading a texture back out of the GPU every frame is not free,
 * and doing it for a picture nobody has asked for would make this
 * feature cost something even when it is switched off.
 */
int bs_server_wants_screen(const BsServer *srv, int screen);

/*
 * Asks whoever is watching a question the controller cannot answer.
 *
 * A 3DS or a Wii U stops and asks for a name, a message or a Mii, and
 * an emulator answers that with a dialog on the desktop it happens to
 * be running on. Streamed to a phone, that dialog is somewhere nobody
 * can see and the game waits for ever.
 *
 * `kind` is BS_PROMPT_TEXT or BS_PROMPT_CHOICE. Returns an id to quote
 * to the two calls below, or 0 when nobody is watching -- in which case
 * the backend should do whatever it did before this existed, because
 * there is nobody to ask.
 *
 * Never blocks. The backend polls, which suits every applet here: they
 * are all asynchronous already, and blocking an emulator's thread on a
 * person typing is how an emulator stops responding.
 */
uint16_t bs_server_prompt(BsServer *srv, int kind, const char *title,
                          const char *const *choices, int n_choices,
                          int max_len, int multiline);

/*
 * 1 when answered, 0 while still waiting, -1 when the person declined or
 * the question was withdrawn. On 1, `text` holds the answer and
 * `choice` the index picked.
 */
int bs_server_prompt_poll(BsServer *srv, uint16_t id,
                          char *text, size_t textlen, int *choice);

/* Withdraws a question the game has stopped waiting for, so it stops
 * being a box somebody is still typing into. */
void bs_server_prompt_cancel(BsServer *srv, uint16_t id);

/* The port the server actually bound, which may not be the one asked
 * for. 0 before it starts. */
uint16_t bs_server_port(const BsServer *srv);

/* 1 while at least one client is connected. */
int bs_server_has_client(const BsServer *srv);

/* How many clients are watching right now. */
int bs_server_clients(const BsServer *srv);

/* Frames sent since the server started. */
uint32_t bs_server_frames(const BsServer *srv);

/*
 * Which of a Wii U's two audio outputs the clients asked for, as a
 * BsAudioSource. Read by the Cemu bridge, which is the only place that
 * has two to choose from; every other backend can ignore it.
 */
int bs_server_audio_source(const BsServer *srv);

#endif /* BOTTOM_SCREEN_SERVER_H */
