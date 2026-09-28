#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>

#include "bs_protocol.h"
#include "stream.h"

typedef struct {
    int physical;
    int code;
} ButtonMap;

static const ButtonMap BUTTONS[] = {
    { 0,  BS_BTN_A },
    { 1,  BS_BTN_B },
    { 2,  BS_BTN_X },
    { 3,  BS_BTN_Y },
    { 6,  BS_BTN_L },
    { 7,  BS_BTN_R },
    { 8,  BS_BTN_ZL },
    { 9,  BS_BTN_ZR },
    { 10, BS_BTN_START },
    { 11, BS_BTN_SELECT },
    { 12, BS_BTN_LEFT },
    { 13, BS_BTN_UP },
    { 14, BS_BTN_RIGHT },
    { 15, BS_BTN_DOWN },
};

/* Raw Wii U SDL joystick button order, validated by Capture2Cloud. */
#define WIIU_BTN_L3 4
#define WIIU_BTN_R3 5

static const int AXIS_CODE[4] = {
    BS_AXIS_LEFT_X,
    BS_AXIS_LEFT_Y,
    BS_AXIS_RIGHT_X,
    BS_AXIS_RIGHT_Y,
};

static SDL_Joystick *g_pad;

static uint8_t g_last_button[
    sizeof(BUTTONS) / sizeof(BUTTONS[0])
];

static int16_t g_last_axis[4];
static int g_primed;
static int g_screen_chord_held;
static int g_screen_toggle_pending;

static int axis_value(int axis)
{
    int v = SDL_JoystickGetAxis(g_pad, axis);

    /*
     * SDL's Wii U backend follows the SDL convention: up is negative.
     * Bottom Screen follows the native controller convention used by
     * the Switch/libdrc clients: up is positive.
     */
    if (axis == 1 || axis == 3)
        v = -v;

    if (v > 32767)
        v = 32767;
    if (v < -32767)
        v = -32767;

    /* Small hardware rest noise is not useful network traffic. */
    if (abs(v) < 1800)
        v = 0;

    /*
     * Quantise very slightly so one ADC count of jitter does not become
     * a TCP event every frame.
     */
    if (v)
        v = (v / 128) * 128;

    return v;
}

int input_init(char *why, size_t why_size)
{
    if (why && why_size)
        why[0] = '\0';

    input_exit();

    SDL_JoystickEventState(SDL_ENABLE);

    if (SDL_NumJoysticks() <= 0) {
        snprintf(why, why_size, "no Wii U GamePad joystick");
        return -1;
    }

    g_pad = SDL_JoystickOpen(0);
    if (!g_pad) {
        snprintf(why, why_size, "SDL_JoystickOpen: %s", SDL_GetError());
        return -1;
    }

    memset(g_last_button, 0, sizeof(g_last_button));
    memset(g_last_axis, 0, sizeof(g_last_axis));
    g_primed = 0;
    g_screen_chord_held = 0;
    g_screen_toggle_pending = 0;

    WHBLogPrintf(
        "input: raw joystick '%s', buttons=%d axes=%d",
        SDL_JoystickName(g_pad),
        SDL_JoystickNumButtons(g_pad),
        SDL_JoystickNumAxes(g_pad));

    return 0;
}

void input_exit(void)
{
    if (g_pad) {
        SDL_JoystickClose(g_pad);
        g_pad = NULL;
    }

    g_primed = 0;
    g_screen_chord_held = 0;
    g_screen_toggle_pending = 0;
}

void input_update(int forward)
{
    if (!g_pad)
        return;

    SDL_JoystickUpdate();

    /*
     * HOME belongs to the local console, so use the two stick clicks for the
     * screen switch.  Latch the chord: holding it for several frames must only
     * toggle once.  Sampling while forwarding is disabled prevents a chord
     * already held in the menu from firing when the menu closes.
     */
    const int screen_chord =
        SDL_JoystickGetButton(g_pad, WIIU_BTN_L3) &&
        SDL_JoystickGetButton(g_pad, WIIU_BTN_R3);

    if (forward &&
        screen_chord &&
        !g_screen_chord_held) {

        g_screen_toggle_pending = 1;
        WHBLogPrintf("input: L3+R3 -> toggle screen");
    }

    g_screen_chord_held = screen_chord;

    for (unsigned i = 0;
         i < sizeof(BUTTONS) / sizeof(BUTTONS[0]);
         ++i) {

        const uint8_t now =
            forward
                ? (SDL_JoystickGetButton(
                       g_pad,
                       BUTTONS[i].physical) ? 1 : 0)
                : 0;

        if (!g_primed || now != g_last_button[i]) {
            stream_send_button(
                BUTTONS[i].code,
                now);

            g_last_button[i] = now;
        }
    }

    for (int i = 0; i < 4; ++i) {
        int v = forward ? axis_value(i) : 0;

        if (!g_primed || v != g_last_axis[i]) {
            stream_send_axis(
                AXIS_CODE[i],
                v);

            g_last_axis[i] =
                (int16_t)v;
        }
    }

    g_primed = 1;
}

int input_take_screen_toggle(void)
{
    const int pending = g_screen_toggle_pending;
    g_screen_toggle_pending = 0;
    return pending;
}
