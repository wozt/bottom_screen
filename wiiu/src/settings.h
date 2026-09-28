#ifndef BS_WIIU_SETTINGS_H
#define BS_WIIU_SETTINGS_H

#include <stdint.h>

#define SETTINGS_HOST_MAX    64
#define SETTINGS_MAX_SERVERS 8

typedef struct {
    char host[SETTINGS_HOST_MAX];
    uint16_t port;
} SavedServer;

typedef struct {
    char host[SETTINGS_HOST_MAX];
    uint16_t port;
    uint8_t screen;

    /* Index in the automatic/low/medium/high/maximum ladder. */
    uint8_t quality[2];

    /* 0 = source, -2 = half native, otherwise a native-size multiple. */
    int8_t receive_scale[2];

    uint8_t volume;
    uint8_t muted;
    uint8_t audio_source;
    uint8_t auto_connect;

    SavedServer servers[SETTINGS_MAX_SERVERS];
    uint8_t server_count;
    uint8_t selected_server;
} Settings;

void settings_load(Settings *out);
int settings_save(const Settings *s, char *why, unsigned why_size);

void settings_host_string(const Settings *s, char *out, unsigned out_size);
int settings_set_host_string(Settings *s, const char *text);

int settings_select_server(Settings *s, int index);
int settings_remember_server(Settings *s);
int settings_remove_server(Settings *s, int index);

#endif
