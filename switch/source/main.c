/*
 * Bottom Screen, on a Switch.
 *
 * Holds the console you are emulating in your hands: the bottom screen
 * arrives on the touchscreen, and the Joy-Cons drive it. The DS and 3DS
 * are the point of this -- a stylus screen on a machine that has one --
 * but the Wii U GamePad works the same way.
 *
 * The protocol, the framing and the decoder are the same files the
 * desktop client uses. Only what is genuinely different lives here: the
 * console's own video block, its pad, its touchscreen and its keyboard.
 */
#include <switch.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bs_protocol.h"
#include "stream.h"

#define SCREEN_W 1280
#define SCREEN_H 720

#define CONFIG_DIR  "sdmc:/switch/bottom_screen"
#define CONFIG_PATH CONFIG_DIR "/server.txt"

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_picture;
static int           g_pic_w, g_pic_h;
static TTF_Font     *g_font, *g_small;
static SDL_AudioDeviceID g_audio;

static char     g_host[64] = "192.168.1.20";
static uint16_t g_port = BS_DEFAULT_PORT;
static char     g_message[192] = "Press A to connect";
static int      g_have_saved_server;

/* ------------------------------------------------------------ settings */

/*
 * One line, host and port. A phone can be typed on; a Switch cannot, so
 * the address is worth keeping even more here than there.
 */
static void load_server(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    char host[64] = "";
    unsigned port = 0;
    if (fscanf(f, "%63s %u", host, &port) == 2 && host[0] && port > 0 && port < 65536) {
        snprintf(g_host, sizeof(g_host), "%s", host);
        g_port = (uint16_t)port;
        g_have_saved_server = 1;
    }
    fclose(f);
}

static void save_server(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    fprintf(f, "%s %u\n", g_host, (unsigned)g_port);
    fclose(f);
}

/* The console's own keyboard, which is the only sane way to type an IP
 * address with a pad in your hands. */
static int ask_text(const char *heading, const char *initial, char *out, size_t outlen)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return 0;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, heading);
    swkbdConfigSetInitialText(&kbd, initial);
    Result rc = swkbdShow(&kbd, out, outlen);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0] != '\0';
}

/* ------------------------------------------------------------- drawing */

static void draw_menu(void);
static void draw_menu_now(void) { draw_menu(); }

static void draw_text(TTF_Font *font, const char *s, int x, int y, SDL_Color colour)
{
    if (!font || !s || !*s)
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, s, colour);
    if (!surface)
        return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_renderer, surface);
    if (t) {
        SDL_Rect dst = { x, y, surface->w, surface->h };
        SDL_RenderCopy(g_renderer, t, NULL, &dst);
        SDL_DestroyTexture(t);
    }
    SDL_FreeSurface(surface);
}

/*
 * The picture keeps its shape exactly and fills whatever is left over.
 *
 * A DS screen is 4:3 and stays 4:3; what is never done is stretching the
 * two axes apart, which is what makes a picture fat. The zoom itself is
 * whatever fraction fits rather than a whole number -- on a 1280 by 720
 * panel, insisting on whole multiples of 256 would throw away a third of
 * the screen to keep pixels square that nobody is inspecting.
 */
static SDL_Rect fit(int w, int h)
{
    if (w <= 0 || h <= 0)
        return (SDL_Rect){ 0, 0, SCREEN_W, SCREEN_H };

    int dw = SCREEN_W;
    int dh = dw * h / w;
    if (dh > SCREEN_H) {
        dh = SCREEN_H;
        dw = dh * w / h;
    }
    return (SDL_Rect){ (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh };
}

static void ensure_texture(int w, int h)
{
    if (g_picture && w == g_pic_w && h == g_pic_h)
        return;
    if (g_picture)
        SDL_DestroyTexture(g_picture);
    g_picture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
    g_pic_w = w;
    g_pic_h = h;
}

/* ------------------------------------------------------------ the pad */

/*
 * Straight across, unlike the phone.
 *
 * A Switch labels its buttons the way every console here does -- A on
 * the right, B at the bottom -- so the button under your thumb already
 * means what it says. The Android client has to swap them, because
 * Android names its buttons the way an Xbox pad is labelled.
 */
static const struct { u64 mask; int code; } BUTTONS[] = {
    { HidNpadButton_A,      BS_BTN_A },
    { HidNpadButton_B,      BS_BTN_B },
    { HidNpadButton_X,      BS_BTN_X },
    { HidNpadButton_Y,      BS_BTN_Y },
    { HidNpadButton_L,      BS_BTN_L },
    { HidNpadButton_R,      BS_BTN_R },
    { HidNpadButton_ZL,     BS_BTN_ZL },
    { HidNpadButton_ZR,     BS_BTN_ZR },
    { HidNpadButton_Plus,   BS_BTN_START },
    { HidNpadButton_Minus,  BS_BTN_SELECT },
    { HidNpadButton_Up,     BS_BTN_UP },
    { HidNpadButton_Down,   BS_BTN_DOWN },
    { HidNpadButton_Left,   BS_BTN_LEFT },
    { HidNpadButton_Right,  BS_BTN_RIGHT },
};

static void send_pad(u64 down, u64 up)
{
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        if (down & BUTTONS[i].mask) stream_send_button(BUTTONS[i].code, 1);
        if (up   & BUTTONS[i].mask) stream_send_button(BUTTONS[i].code, 0);
    }
}

static void send_sticks(const HidAnalogStickState *left,
                        const HidAnalogStickState *right)
{
    static int lx, ly, rx, ry;
    /* Only on change: a stick at rest would otherwise fill the link with
     * events saying nothing happened. */
    if (left->x != lx)  { lx = left->x;  stream_send_axis(BS_AXIS_LEFT_X, lx); }
    if (left->y != ly)  { ly = left->y;  stream_send_axis(BS_AXIS_LEFT_Y, ly); }
    if (right->x != rx) { rx = right->x; stream_send_axis(BS_AXIS_RIGHT_X, rx); }
    if (right->y != ry) { ry = right->y; stream_send_axis(BS_AXIS_RIGHT_Y, ry); }
}

/*
 * A finger on the picture becomes a stylus on the console.
 *
 * The coordinates sent are in the space the server announced, not the
 * console's native size: the stream is larger whenever the emulator
 * renders at a higher internal resolution, and dividing by the native
 * size instead would put every tap wrong by exactly that scale.
 */
static void send_touch(const SDL_Rect *dst, const StreamInfo *info)
{
    static int touching;

    HidTouchScreenState st = {0};
    if (!hidGetTouchScreenStates(&st, 1) || st.count == 0) {
        if (touching) {
            touching = 0;
            stream_send_touch(BS_INPUT_TOUCH_UP, 0, 0);
        }
        return;
    }

    const int tx = (int)st.touches[0].x;
    const int ty = (int)st.touches[0].y;
    if (tx < dst->x || tx >= dst->x + dst->w ||
        ty < dst->y || ty >= dst->y + dst->h) {
        /* Outside the picture: not a stylus press, and treating it as
         * one would put the pen on the far edge of the screen. */
        if (touching) {
            touching = 0;
            stream_send_touch(BS_INPUT_TOUCH_UP, 0, 0);
        }
        return;
    }

    const int x = (tx - dst->x) * info->width / dst->w;
    const int y = (ty - dst->y) * info->height / dst->h;
    stream_send_touch(touching ? BS_INPUT_TOUCH_MOVE : BS_INPUT_TOUCH_DOWN, x, y);
    touching = 1;
}

/* -------------------------------------------------------------- sound */

static void audio_callback(void *user, Uint8 *out, int len)
{
    (void)user;
    const int frames = len / (int)(sizeof(int16_t) * 2);
    const int got = stream_take_audio((int16_t *)out, frames);
    /* Silence rather than the previous buffer again: a repeat is a click,
     * and a gap in a stream that is behind is the honest sound. */
    if (got < frames)
        memset(out + (size_t)got * sizeof(int16_t) * 2, 0,
               (size_t)(frames - got) * sizeof(int16_t) * 2);
}

static void open_audio(void)
{
    if (g_audio)
        return;
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = 48000;                 /* what Opus carries */
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_callback;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (g_audio)
        SDL_PauseAudioDevice(g_audio, 0);
}

/* --------------------------------------------------------------- menu */

/*
 * Tries the address on screen. Also run once at startup when one was
 * remembered: on a console you launch this to play, not to look at a
 * menu, and a failure lands back here with A ready to try again.
 */
static void try_connect(void)
{
    char err[160] = "";
    snprintf(g_message, sizeof(g_message), "Connecting to %s:%u...",
             g_host, (unsigned)g_port);
    draw_menu_now();

    if (stream_connect(g_host, g_port, err, sizeof(err)) == 0) {
        open_audio();
        StreamInfo info;
        stream_info(&info);
        snprintf(g_message, sizeof(g_message), "%dx%d, %s",
                 info.width, info.height, stream_decoder_name());
    } else {
        snprintf(g_message, sizeof(g_message), "%s", err);
    }
}

static void draw_menu(void)
{
    const SDL_Color white = { 235, 235, 235, 255 };
    const SDL_Color grey  = { 150, 150, 150, 255 };

    SDL_SetRenderDrawColor(g_renderer, 16, 16, 20, 255);
    SDL_RenderClear(g_renderer);

    draw_text(g_font, "Bottom Screen", 80, 90, white);

    char line[128];
    snprintf(line, sizeof(line), "%s : %u", g_host, (unsigned)g_port);
    draw_text(g_font, line, 80, 190, white);

    draw_text(g_small, "A  connect        X  address        Y  port", 80, 300, grey);
    draw_text(g_small, "+  quit", 80, 340, grey);
    draw_text(g_small, g_message, 80, 430, grey);

    SDL_RenderPresent(g_renderer);
}

static void draw_playing(const StreamInfo *info, SDL_Rect *dst_out)
{
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);

    if (info->width > 0 && info->height > 0) {
        ensure_texture(info->width, info->height);

        if (g_picture) {
            void *pixels = NULL;
            int pitch = 0;
            if (SDL_LockTexture(g_picture, NULL, &pixels, &pitch) == 0) {
                uint8_t *y = pixels;
                uint8_t *u = y + (size_t)pitch * info->height;
                uint8_t *v = u + (size_t)(pitch / 2) * (info->height / 2);
                stream_take_frame(y, u, v, pitch, pitch / 2,
                                  info->width, info->height);
                SDL_UnlockTexture(g_picture);
            }
            SDL_Rect dst = fit(info->width, info->height);
            SDL_RenderCopy(g_renderer, g_picture, NULL, &dst);
            *dst_out = dst;
        }
    }

    /*
     * The decoder's name and its output, in the corner.
     *
     * A stream that is connected and a stream that is arriving look
     * identical when both are black, and the difference between the
     * console's video block and a software fallback is the difference
     * between a cool machine and a hot one. Neither is visible any other
     * way.
     */
    char hud[96];
    snprintf(hud, sizeof(hud), "%s  %ux%u  %u frames",
             stream_decoder_name(), (unsigned)info->width,
             (unsigned)info->height, (unsigned)stream_frames());
    const SDL_Color faint = { 120, 200, 120, 255 };
    draw_text(g_small, hud, 16, 12, faint);

    SDL_RenderPresent(g_renderer);
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    socketInitializeDefault();
    plInitialize(PlServiceType_User);
    romfsInit();
    load_server();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
        goto done;
    if (TTF_Init() != 0)
        goto done;

    g_window = SDL_CreateWindow("Bottom Screen", 0, 0, SCREEN_W, SCREEN_H, 0);
    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    if (!g_window || !g_renderer)
        goto done;

    /* The console's own font, so nothing has to be shipped with the
     * program and the text looks like the rest of the system. */
    PlFontData font_data;
    if (R_SUCCEEDED(plGetSharedFontByType(&font_data, PlSharedFontType_Standard))) {
        g_font = TTF_OpenFontRW(
            SDL_RWFromMem(font_data.address, font_data.size), 1, 40);
        g_small = TTF_OpenFontRW(
            SDL_RWFromMem(font_data.address, font_data.size), 1, 26);
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();

    SDL_Rect picture_rect = { 0, 0, SCREEN_W, SCREEN_H };

    if (g_have_saved_server)
        try_connect();

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        const u64 up   = padGetButtonsUp(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (!stream_connected()) {
            char typed[64];
            if (down & HidNpadButton_X) {
                if (ask_text("Server address", g_host, typed, sizeof(typed))) {
                    snprintf(g_host, sizeof(g_host), "%s", typed);
                    save_server();
                }
            } else if (down & HidNpadButton_Y) {
                char current[16];
                snprintf(current, sizeof(current), "%u", (unsigned)g_port);
                if (ask_text("Port", current, typed, sizeof(typed))) {
                    int p = atoi(typed);
                    if (p > 0 && p < 65536) {
                        g_port = (uint16_t)p;
                        save_server();
                    }
                }
            } else if (down & HidNpadButton_A) {
                try_connect();
            }
            draw_menu();
            continue;
        }

        StreamInfo info;
        stream_info(&info);

        send_pad(down, up);
        HidAnalogStickState left = padGetStickPos(&pad, 0);
        HidAnalogStickState right = padGetStickPos(&pad, 1);
        send_sticks(&left, &right);
        send_touch(&picture_rect, &info);

        draw_playing(&info, &picture_rect);

        if (!stream_connected()) {
            snprintf(g_message, sizeof(g_message), "%s", stream_last_error());
            stream_disconnect();
        }
    }

done:
    stream_disconnect();
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    if (g_picture) SDL_DestroyTexture(g_picture);
    if (g_font) TTF_CloseFont(g_font);
    if (g_small) TTF_CloseFont(g_small);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    TTF_Quit();
    SDL_Quit();
    romfsExit();
    plExit();
    socketExit();
    return 0;
}
