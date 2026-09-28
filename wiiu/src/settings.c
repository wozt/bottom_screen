#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <whb/sdcard.h>

#include "bs_protocol.h"

#define SETTINGS_DIR  "/wiiu/apps/bottom_screen"
#define SETTINGS_LEAF SETTINGS_DIR "/bottom_screen.cfg"

static const Settings DEFAULTS = {
    .host = "0.0.0.0",
    .port = BS_DEFAULT_PORT,
    .screen = BS_SCREEN_BOTTOM,
    .quality = { 0, 0 },
    .receive_scale = { 0, 0 },
    .volume = 100,
    .muted = 0,
    .audio_source = BS_AUDIO_BOTH,
    .auto_connect = 1,
    .server_count = 0,
    .selected_server = 0,
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

static void copy_host(char *out, const char *in)
{
    const char *src = in ? in : "";
    size_t n = 0;
    while (n + 1 < SETTINGS_HOST_MAX && src[n])
        n++;
    memmove(out, src, n);
    out[n] = '\0';
}

static int valid_host(const char *text)
{
    if (!text || !text[0] || strlen(text) >= SETTINGS_HOST_MAX)
        return 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (isspace(*p) || *p == '/' || *p == '\\')
            return 0;
    }
    return 1;
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

    char line[192];

    while (fgets(line, sizeof(line), f)) {
        char host[SETTINGS_HOST_MAX];
        unsigned port = 0;
        int v = 0;

        if (sscanf(line, " host = %63s", host) == 1 && valid_host(host)) {
            copy_host(out->host, host);
        } else if (sscanf(line, " port = %u", &port) == 1 &&
                   port > 0 && port <= 65535) {
            out->port = (uint16_t)port;
        } else if (sscanf(line, " screen = %d", &v) == 1 &&
                   v >= 0 && v < BS_SCREEN_COUNT) {
            out->screen = (uint8_t)v;
        } else if (sscanf(line, " quality_bottom = %d", &v) == 1 &&
                   v >= 0 && v <= 4) {
            out->quality[BS_SCREEN_BOTTOM] = (uint8_t)v;
        } else if (sscanf(line, " quality_top = %d", &v) == 1 &&
                   v >= 0 && v <= 4) {
            out->quality[BS_SCREEN_TOP] = (uint8_t)v;
        } else if (sscanf(line, " scale_bottom = %d", &v) == 1 &&
                   (v == -2 || (v >= 0 && v <= 4))) {
            out->receive_scale[BS_SCREEN_BOTTOM] = (int8_t)v;
        } else if (sscanf(line, " scale_top = %d", &v) == 1 &&
                   (v == -2 || (v >= 0 && v <= 4))) {
            out->receive_scale[BS_SCREEN_TOP] = (int8_t)v;
        } else if (sscanf(line, " volume = %d", &v) == 1 &&
                   v >= 0 && v <= 100) {
            out->volume = (uint8_t)v;
        } else if (sscanf(line, " muted = %d", &v) == 1) {
            out->muted = (uint8_t)(v != 0);
        } else if (sscanf(line, " audio_source = %d", &v) == 1 &&
                   v >= BS_AUDIO_BOTH && v <= BS_AUDIO_PAD) {
            out->audio_source = (uint8_t)v;
        } else if (sscanf(line, " auto_connect = %d", &v) == 1) {
            out->auto_connect = (uint8_t)(v != 0);
        } else if (sscanf(line, " selected_server = %d", &v) == 1 &&
                   v >= 0 && v < SETTINGS_MAX_SERVERS) {
            out->selected_server = (uint8_t)v;
        } else if (sscanf(line, " server = %63s %u", host, &port) == 2 &&
                   valid_host(host) && port > 0 && port <= 65535 &&
                   out->server_count < SETTINGS_MAX_SERVERS) {
            SavedServer *srv = &out->servers[out->server_count++];
            copy_host(srv->host, host);
            srv->port = (uint16_t)port;
        }
    }

    fclose(f);

    if (out->server_count == 0 && strcmp(out->host, "0.0.0.0") != 0)
        settings_remember_server(out);

    if (out->server_count > 0) {
        if (out->selected_server >= out->server_count)
            out->selected_server = 0;
        settings_select_server(out, out->selected_server);
    }
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
            "host = %s\n"
            "port = %u\n"
            "screen = %u\n"
            "quality_bottom = %u\n"
            "quality_top = %u\n"
            "scale_bottom = %d\n"
            "scale_top = %d\n"
            "volume = %u\n"
            "muted = %u\n"
            "audio_source = %u\n"
            "auto_connect = %u\n"
            "selected_server = %u\n",
            s->host, s->port, s->screen,
            s->quality[BS_SCREEN_BOTTOM],
            s->quality[BS_SCREEN_TOP],
            s->receive_scale[BS_SCREEN_BOTTOM],
            s->receive_scale[BS_SCREEN_TOP],
            s->volume, s->muted, s->audio_source,
            s->auto_connect, s->selected_server);

    for (unsigned i = 0; i < s->server_count; ++i) {
        fprintf(f, "server = %s %u\n",
                s->servers[i].host,
                s->servers[i].port);
    }

    fclose(f);

    if (why_size)
        why[0] = '\0';

    return 0;
}

void settings_host_string(const Settings *s,
                          char *out,
                          unsigned out_size)
{
    snprintf(out, out_size, "%s", s->host);
}

int settings_set_host_string(Settings *s, const char *text)
{
    if (!valid_host(text))
        return -1;
    copy_host(s->host, text);
    return 0;
}

int settings_select_server(Settings *s, int index)
{
    if (!s || index < 0 || index >= s->server_count)
        return -1;
    s->selected_server = (uint8_t)index;
    copy_host(s->host, s->servers[index].host);
    s->port = s->servers[index].port;
    return 0;
}

int settings_remember_server(Settings *s)
{
    if (!s || !valid_host(s->host) || s->port == 0)
        return -1;

    int at = -1;
    for (int i = 0; i < s->server_count; ++i) {
        if (strcmp(s->servers[i].host, s->host) == 0 &&
            s->servers[i].port == s->port) {
            at = i;
            break;
        }
    }

    if (at < 0) {
        if (s->server_count >= SETTINGS_MAX_SERVERS)
            return -1;
        at = s->server_count++;
        copy_host(s->servers[at].host, s->host);
        s->servers[at].port = s->port;
    }

    s->selected_server = (uint8_t)at;
    return at;
}

int settings_remove_server(Settings *s, int index)
{
    if (!s || index < 0 || index >= s->server_count)
        return -1;

    for (int i = index; i + 1 < s->server_count; ++i)
        s->servers[i] = s->servers[i + 1];

    s->server_count--;
    if (s->server_count == 0) {
        s->selected_server = 0;
    } else {
        if (s->selected_server >= s->server_count)
            s->selected_server = s->server_count - 1;
        settings_select_server(s, s->selected_server);
    }
    return 0;
}
