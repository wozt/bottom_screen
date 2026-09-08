/*
 * The Switch homebrew's own network half, run on a desktop.
 *
 * switch/source/stream.c contains no libnx: it is sockets, the shared
 * protocol, and a decoder. So the part of the console client that talks
 * to the server can be compiled here and driven against a real one,
 * which is the only way anything about that client gets tested without
 * the console in hand.
 *
 * It exists because of a specific failure. Switching screens on the
 * console appeared to do nothing at all -- and the reason was not in the
 * menu, or the message, or the server. It was that this client was the
 * only one of the three that did not rebuild its decoder when the
 * picture changed shape, and the console decodes in hardware, which does
 * not reconfigure itself. Every part worked; the picture stopped
 * changing. Counting the frames actually decoded, at the size they were
 * actually decoded at, is what separates those two.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "stream.h"

static uint8_t y[2048 * 1536], u[1024 * 768], v[1024 * 768];

/* Frames the console would have drawn, over the given milliseconds. */
static int drain(int ms)
{
    int got = 0;
    for (int t = 0; t < ms / 10; t++) {
        int w = 0, h = 0;
        if (stream_picture_size(&w, &h) && (size_t)w * h <= sizeof(y) &&
            stream_take_frame(y, u, v, w, w / 2, w, h))
            got++;
        usleep(10000);
    }
    return got;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: switch_client PORT\n");
        return 2;
    }

    char err[256] = "";
    if (stream_connect("127.0.0.1", (uint16_t)atoi(argv[1]), err, sizeof(err)) != 0) {
        printf("FAIL: %s\n", err);
        return 1;
    }

    StreamInfo info;
    stream_info(&info);
    int w = 0, h = 0, fail = 0;

    const int bottom = drain(1500);
    stream_picture_size(&w, &h);
    printf("  bottom: %d frames at %dx%d\n", bottom, w, h);
    if (bottom == 0) {
        printf("  nothing decoded at all\n");
        fail = 1;
    }
    const int bw = w, bh = h;

    if (!(stream_screens() & (1 << BS_SCREEN_TOP))) {
        printf("  this server has no top screen, so there is nothing to switch to\n");
        stream_disconnect();
        printf(fail ? "FAIL\n" : "PASS\n");
        return fail;
    }

    stream_send_screen(BS_SCREEN_TOP);
    usleep(1500 * 1000);
    const int top = drain(2500);
    stream_picture_size(&w, &h);
    printf("  top: %d frames at %dx%d\n", top, w, h);
    /*
     * Both halves matter. Frames arriving says the switch happened;
     * the size having changed says they are the other screen and not
     * the same one still coming through -- a 3DS is 320x240 below and
     * 400x240 above, and nothing else distinguishes them from here.
     */
    if (top == 0) {
        printf("  the picture stopped: the decoder did not follow the new size\n");
        fail = 1;
    } else if (w == bw && h == bh && info.console == BS_CONSOLE_3DS) {
        printf("  the size never changed, so this is still the bottom screen\n");
        fail = 1;
    }

    stream_send_screen(BS_SCREEN_BOTTOM);
    usleep(1500 * 1000);
    const int back = drain(2000);
    stream_picture_size(&w, &h);
    printf("  back: %d frames at %dx%d\n", back, w, h);
    if (back == 0 || w != bw || h != bh) {
        printf("  it did not come back to the screen it started on\n");
        fail = 1;
    }

    stream_disconnect();
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
