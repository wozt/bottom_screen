#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <whb/sdcard.h>

#include "bs_protocol.h"

#define SETTINGS_DIR  "/wiiu/apps/bottom_screen"
#define SETTINGS_LEAF SETTINGS_DIR "/bottom_screen.cfg"

static const Settings DEFAULTS = {
    .host = { 0, 0, 0, 0 },
    .port = BS_DEFAULT_PORT,
    .screen = BS_SCREEN_BOTTOM,
};

static int settings_path(char *out, unsigned out_size)
{
    if (!WHBMountSdCard())
        return -1;

    const char *root = WHBGetSdCardMountPath();
    if (!root)
        return -1;

    snprintf(out, out_size, "%s%s", root, SETTINGS_LEAF);
    return 0;
}

void settings_load(Settings *out)
{
    *out = DEFAULTS;

    char path[256];
    if (settings_path(path, sizeof(path)) != 0)
        return;

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[128];

    while (fgets(line, sizeof(line), f)) {
        unsigned a, b, c, d, v;

        if (sscanf(line, " host = %u.%u.%u.%u",
                   &a, &b, &c, &d) == 4 &&
            a < 256 && b < 256 && c < 256 && d < 256) {
            out->host[0] = a;
            out->host[1] = b;
            out->host[2] = c;
            out->host[3] = d;
        } else if (sscanf(line, " port = %u", &v) == 1 &&
                   v > 0 && v <= 65535) {
            out->port = (uint16_t)v;
        } else if (sscanf(line, " screen = %u", &v) == 1 &&
                   v < BS_SCREEN_COUNT) {
            out->screen = (uint8_t)v;
        }
    }

    fclose(f);
}

int settings_save(const Settings *s, char *why, unsigned why_size)
{
    char path[256];

    if (settings_path(path, sizeof(path)) != 0) {
        snprintf(why, why_size, "SD card unavailable");
        return -1;
    }

    const char *root = WHBGetSdCardMountPath();
    if (root) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s%s", root, SETTINGS_DIR);
        mkdir(dir, 0777);
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(why, why_size, "cannot write bottom_screen.cfg");
        return -1;
    }

    fprintf(f,
            "# Bottom Screen Wii U console client\n"
            "host = %u.%u.%u.%u\n"
            "port = %u\n"
            "screen = %u\n",
            s->host[0], s->host[1],
            s->host[2], s->host[3],
            s->port,
            s->screen);

    fclose(f);

    if (why_size)
        why[0] = '\0';

    return 0;
}

void settings_host_string(const Settings *s,
                          char *out,
                          unsigned out_size)
{
    snprintf(out, out_size, "%u.%u.%u.%u",
             s->host[0], s->host[1],
             s->host[2], s->host[3]);
}

int settings_set_host_string(Settings *s, const char *text)
{
    unsigned a, b, c, d;
    char tail;

    if (!text ||
        sscanf(text, "%u.%u.%u.%u%c",
               &a, &b, &c, &d, &tail) != 4)
        return -1;

    if (a > 255 || b > 255 || c > 255 || d > 255)
        return -1;

    s->host[0] = a;
    s->host[1] = b;
    s->host[2] = c;
    s->host[3] = d;

    return 0;
}
