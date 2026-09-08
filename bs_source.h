#ifndef BOTTOM_SCREEN_SOURCE_H
#define BOTTOM_SCREEN_SOURCE_H

#include <stdint.h>
#include "bs_encoder.h"
#include "bs_protocol.h"

/*
 * Where a bottom screen comes from, and where its input goes back to.
 *
 * This is the seam between bottom_screen_server and the emulators. A
 * struct of function pointers plus a void* is the C way to say what the
 * plan called BottomScreenSource, and it is exactly what a C++ backend
 * can fill in: the backend is a .cpp file that calls the emulator's C++
 * API and assigns these slots. Nothing above this header knows whether
 * it is talking to a DS, a 3DS or a Wii U.
 *
 * The test pattern in testpattern.c implements it too, which is what
 * lets the whole pipeline be built and measured before any emulator is
 * touched.
 */

typedef struct {
    int       width;
    int       height;
    int       fps;
    BsPixFmt  pixfmt;
    BsConsole console;

    /* Sound, or zero for a source that has none. A source without audio
     * is normal -- the test pattern had none for weeks -- so it is
     * absence rather than an error, and the client draws no volume
     * control instead of one that does nothing. */
    int       audio_rate;      /* Hz, 0 = silent source */
    int       audio_channels;  /* 1 or 2 */
} BsSourceInfo;

typedef struct BsSource BsSource;

struct BsSource {
    void *self;

    void (*get_info)(void *self, BsSourceInfo *out);

    /*
     * Returns the next frame, blocking until one is ready, or NULL when
     * the source has ended. The pointer stays valid until the next
     * acquire on the same source -- no copy is made here, because at 60
     * Hz an avoidable copy of every frame is exactly the kind of cost
     * this design exists to avoid.
     *
     * timestamp_us is when the frame was produced, for latency
     * accounting; see bs_now_us in bs_net.h.
     */
    const uint8_t *(*acquire)(void *self, int *stride, uint32_t *timestamp_us);

    /* Input travelling the other way. A source that cannot map an event
     * ignores it rather than failing: the DS has no ZL, and a client
     * built for a shared protocol will still occasionally send one. */
    void (*touch)(void *self, BsInputType type, int x, int y);
    void (*button)(void *self, BsButton code, int pressed);
    void (*axis)(void *self, BsAxis code, int value);

    /* Optional. Makes a blocked acquire return NULL so the server can
     * shut down. A source whose acquire always returns within a frame
     * period -- the test pattern -- can leave this NULL. */
    void (*unblock)(void *self);

    /*
     * Drains up to max_frames of interleaved 16-bit samples the source
     * has queued, returning how many it gave. Optional: a silent source
     * leaves it NULL.
     *
     * Pull rather than push, matching the video: the server asks when it
     * is ready, and a source that produces faster than the link can
     * carry drops its oldest rather than growing a queue nobody drains.
     */
    int (*take_audio)(void *self, int16_t *out, int max_frames);

    void (*destroy)(void *self);
};

/* The synthetic source: a moving pattern at the given console's native
 * size, for building and measuring the pipeline without an emulator. */
BsSource *bs_testpattern_create(BsConsole console, int fps);

/* The same, for the machine's other screen: the top one on a DS or 3DS,
 * the television picture on a Wii U. Drawn in a different colour and
 * without a touch crosshair, and silent -- the sound comes from the
 * bottom source whichever screen is being watched. */
BsSource *bs_testpattern_create_top(BsConsole console, int fps);

#endif /* BOTTOM_SCREEN_SOURCE_H */
