#ifndef BS_WIIU_SETTINGS_H
#define BS_WIIU_SETTINGS_H

#include <stdint.h>

typedef struct {
    uint8_t host[4];
    uint16_t port;
    uint8_t screen;
} Settings;

void settings_load(Settings *out);
int settings_save(const Settings *s, char *why, unsigned why_size);

void settings_host_string(const Settings *s, char *out, unsigned out_size);
int settings_set_host_string(Settings *s, const char *text);

#endif
