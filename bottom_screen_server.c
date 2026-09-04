#include "bs_protocol.h"
#include "bs_server.h"
#include "bs_source.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/*
 * The standalone server: argument parsing over bs_server, nothing more.
 *
 * All the work is in bs_server.c, which is also what melonDS links, so
 * the two cannot drift apart. This binary exists to serve the synthetic
 * test pattern -- for measuring the pipeline, and for developing a
 * client without an emulator running.
 */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static BsConsole parse_console(const char *s)
{
    if (!strcasecmp(s, "ds"))   return BS_CONSOLE_DS;
    if (!strcasecmp(s, "3ds"))  return BS_CONSOLE_3DS;
    if (!strcasecmp(s, "wiiu")) return BS_CONSOLE_WIIU;
    return 0;
}

static void usage(void)
{
    printf(
"bottom_screen_server -- streams a console's bottom screen\n"
"\n"
"  --console ds|3ds|wiiu   which machine's screen to serve (default ds)\n"
"  --port N                listen port (default %d)\n"
"  --fps N                 frames per second (default 60)\n"
"  --bitrate N             bits/s; 0 derives one from the resolution\n"
"  --encoder NAME          libx264 (default), h264_vaapi, h264_nvenc\n"
"  --help\n"
"\n"
"This binary serves a synthetic test pattern. The emulator backends use\n"
"the same server code as a library.\n", BS_DEFAULT_PORT);
}

int main(int argc, char **argv)
{
    BsConsole console = BS_CONSOLE_DS;
    BsServerConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    int fps = 60;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--console") && next) { console = parse_console(next); i++; }
        else if (!strcmp(a, "--port") && next)    { cfg.port = (uint16_t)atoi(next); i++; }
        else if (!strcmp(a, "--fps") && next)     { fps = atoi(next); i++; }
        else if (!strcmp(a, "--bitrate") && next) { cfg.bitrate = atoi(next); i++; }
        else if (!strcmp(a, "--encoder") && next) { cfg.encoder = next; i++; }
        else { fprintf(stderr, "unknown argument: %s\n", a); usage(); return 1; }
    }
    if (!console) { fprintf(stderr, "unknown console\n"); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    BsSource *source = bs_testpattern_create(console, fps);
    if (!source) { fprintf(stderr, "cannot create test pattern\n"); return 1; }

    char err[256] = "";
    BsServer *srv = bs_server_create(source, &cfg, err, sizeof(err));
    if (!srv) {
        fprintf(stderr, "%s\n", err);
        source->destroy(source->self);
        free(source);
        return 1;
    }

    /* The server runs on its own thread; this one only waits for a
     * signal. Polling rather than pausing keeps the shutdown path the
     * same one the emulator backend uses. */
    while (!g_stop) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("\nstopping\n");
    bs_server_destroy(srv);
    source->destroy(source->self);
    free(source);
    return 0;
}
