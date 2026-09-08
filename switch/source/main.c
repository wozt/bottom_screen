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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bs_protocol.h"
#include "stream.h"
#include "ui.h"
#include "vpad.h"

#define SCREEN_W 1280
#define SCREEN_H 720

#define CONFIG_DIR  "sdmc:/switch/bottom_screen"
#define CONFIG_PATH   CONFIG_DIR "/server.txt"
#define SETTINGS_PATH CONFIG_DIR "/settings.txt"
/*
 * One arrangement per console, so a DS layout and a Wii U one do not
 * overwrite each other: the two do not even have the same controls.
 * Semicolon-separated "anchor:x,y", written by vpad itself, so a file
 * from a build with different anchors is read as the entries it
 * recognises rather than rejected whole.
 */
#define LAYOUT_PATH_FMT CONFIG_DIR "/layout_%d.txt"
/*
 * Stamped on every saved arrangement.
 *
 * Positions are meaningful only against the layout that produced them.
 * The first version of this pad borrowed a full-screen arrangement and
 * squeezed it into the side bands, which piled every control on one
 * spot -- and a file written then would go on doing that for ever,
 * because a loaded arrangement is somebody's own and the defaults do
 * not touch it. A file without the current stamp is ignored, so the
 * fix arrives without anyone having to know a file exists.
 */
#define LAYOUT_VERSION "v2"

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_picture;
static int           g_pic_w, g_pic_h;
static TTF_Font     *g_font, *g_small;
static SDL_AudioDeviceID g_audio;

static char     g_host[64] = "192.168.1.20";
static uint16_t g_port = BS_DEFAULT_PORT;
static char     g_message[192] = "";
static int      g_have_saved_server;

/*
 * The addresses used before, most recent first.
 *
 * The other two clients both keep a list, because each emulator serves
 * its own page on its own port and moving between them by retyping an
 * address on a console keyboard is a punishment. One line each, in the
 * same file, so an older build reading it still finds its address on
 * the first line.
 */
#define MAX_SERVERS 8
typedef struct { char host[64]; uint16_t port; } SavedServer;
static SavedServer g_servers[MAX_SERVERS];
static int         g_server_count;
static int         g_server_at;

/*
 * The same palette as capture2cloud's own Switch client: a blue accent
 * for whatever is selected, dim grey for values, green for a stream
 * that is running. Two programs that live on the same console should
 * not need two sets of manners.
 */
static const SDL_Color COL_TEXT     = {235, 235, 240, 255};
static const SDL_Color COL_DIM      = {150, 150, 160, 255};
static const SDL_Color COL_ACCENT   = {120, 190, 255, 255};
static const SDL_Color COL_GOOD     = { 90, 210, 130, 255};
static const SDL_Color COL_BAD      = {235, 110, 110, 255};
static const SDL_Color COL_SELECTED = { 45,  85, 140, 255};
static const SDL_Color COL_ROW      = { 38,  40,  48, 170};

/* Rows, because a console is driven with a pad: up and down to choose,
 * A to act. A pointer would be the wrong shape entirely. */
typedef enum { ROW_ACTION, ROW_VALUE, ROW_INFO, ROW_HEADING } RowKind;

/*
 * What a row is, rather than where it sits.
 *
 * The menu used to act on `g_selected == 2`, which was the disconnect
 * row until a Wii U gained a "sound from" row above it -- and then
 * choosing the sound output disconnected instead. Rows are added per
 * console, so their positions are not something to hard-code.
 */
typedef enum {
    ROWID_NONE = 0, ROWID_SIZE, ROWID_VOLUME, ROWID_QUALITY,
    ROWID_AUDIO_SOURCE, ROWID_HOME, ROWID_DISCONNECT, ROWID_DECODER,
    ROWID_ADDRESS, ROWID_PORT, ROWID_SAVED, ROWID_CONNECT, ROWID_BUTTONS,
    ROWID_STATS, ROWID_PAD_EDIT, ROWID_PAD_RESET, ROWID_PAD_COLOUR,
    ROWID_PAD_OPACITY, ROWID_STICK_L, ROWID_STICK_R, ROWID_SCREEN
} RowId;

typedef struct {
    RowKind kind;
    RowId   id;
    const char *label;
    char value[64];
} MenuRow;

/* The row the cursor is on, by identity. */

static RowId selected_id(void);

/*
 * Room for every row any console can ask for, which is fifteen: a Wii U
 * with the on-screen pad shown adds six of its own to the nine that are
 * always there. It was twelve, and turning the pad on wrote three rows
 * past the end -- which quit the application on the spot and crashed it
 * again on every attempt to reopen the menu.
 *
 * The count is not the real fix. add_row is: a row beyond the end goes
 * to a scratch entry that is never drawn, so adding one later can cost
 * a missing line but not memory somebody else was using.
 */
static MenuRow g_rows[24];
static MenuRow g_row_overflow;

static int     g_row_count;
static int     g_selected;

static MenuRow *add_row(void)
{
    if (g_row_count >= (int)(sizeof(g_rows) / sizeof(g_rows[0])))
        return &g_row_overflow;
    return &g_rows[g_row_count++];
}

/*
 * Up and down move between things that can be chosen. A heading is a
 * label, not a row: landing on one and having left and right do nothing
 * reads as the menu having stopped responding.
 */
static void move_selection(int delta)
{
    if (g_row_count <= 0)
        return;
    int at = g_selected;
    for (int guard = 0; guard < g_row_count; guard++) {
        at = (at + delta + g_row_count) % g_row_count;
        if (g_rows[at].kind != ROW_HEADING) {
            g_selected = at;
            return;
        }
    }
}

/* The first thing that can be chosen, for when a menu is rebuilt and
 * the cursor was sitting on a heading or past the end. */
static void settle_selection(void)
{
    if (g_selected >= g_row_count)
        g_selected = g_row_count - 1;
    if (g_selected < 0)
        g_selected = 0;
    if (g_row_count > 0 && g_rows[g_selected].kind == ROW_HEADING)
        move_selection(1);
}


/* Settings while playing, reached by holding START and SELECT. */
static int g_menu_open;
static int g_receive_scale;      /* 0 = as rendered, N = N x native */
static int g_volume = 100;
static int g_muted;
static int g_show_buttons;       /* off until asked for */
static int g_show_stats;         /* likewise: wanted when something is wrong */
/* BS_AUDIO_BOTH until somebody says otherwise. */
static int g_audio_source;

/*
 * Which of the machine's two screens is being watched.
 *
 * Not saved with the rest of the settings, unlike almost everything
 * here. The bottom screen is what this is for, and coming back days
 * later to the television picture because of a choice made once is a
 * worse surprise than picking it again. It also resets on every
 * connection, because the server puts a new client on the bottom screen
 * whatever this one last showed.
 */
static int g_screen;

/* The ladder the web and Android clients offer, in the same words. */
static const struct { const char *label; int bitrate; } QUALITY[] = {
    { "automatic",         0 },
    { "low  ~400 kbit/s",  400000 },
    { "medium  ~1 Mbit/s", 1000000 },
    { "high  ~2.5 Mbit/s", 2500000 },
    { "maximum  ~6 Mbit/s", 6000000 },
};
static int g_quality;
/* The pad's own settings, held here because they are read before the pad
 * exists and applied once it does. */
static int g_pad_colour, g_pad_opacity = 100, g_stick_l, g_stick_r = 1;

static void native_size(int console, int *w, int *h);
static void screen_native_size(int console, int screen, int *w, int *h);

/* ------------------------------------------------------------ settings */

/*
 * One line, host and port. A phone can be typed on; a Switch cannot, so
 * the address is worth keeping even more here than there.
 */
/* A breadcrumb on the SD card, readable from the host. The console has
 * nowhere else to say where it got to. */
static void load_server(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    char host[64];
    unsigned port;
    while (g_server_count < MAX_SERVERS &&
           fscanf(f, "%63s %u", host, &port) == 2 &&
           host[0] && port > 0 && port < 65536) {
        snprintf(g_servers[g_server_count].host,
                 sizeof(g_servers[g_server_count].host), "%s", host);
        g_servers[g_server_count].port = (uint16_t)port;
        g_server_count++;
    }
    fclose(f);

    /* The first line is the one used last, which is the one to offer. */
    if (g_server_count > 0) {
        snprintf(g_host, sizeof(g_host), "%s", g_servers[0].host);
        g_port = g_servers[0].port;
        g_have_saved_server = 1;
    }
}

/* Moves the current address to the front, without duplicating it. */
static void remember_server(void)
{
    int at = -1;
    for (int i = 0; i < g_server_count; i++)
        if (strcmp(g_servers[i].host, g_host) == 0 && g_servers[i].port == g_port)
            at = i;

    if (at < 0) {
        if (g_server_count < MAX_SERVERS)
            g_server_count++;
        at = g_server_count - 1;
    }
    for (int i = at; i > 0; i--)
        g_servers[i] = g_servers[i - 1];

    snprintf(g_servers[0].host, sizeof(g_servers[0].host), "%s", g_host);
    g_servers[0].port = g_port;
    g_server_at = 0;
}

/*
 * Everything the menu can change, on the card.
 *
 * A separate file from the addresses, and one `key value` a line, so a
 * setting added later is read by an older build as a line it does not
 * know rather than as a corrupt address.
 */
static void save_settings(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "volume %d\n",       g_volume);
    fprintf(f, "muted %d\n",        g_muted);
    fprintf(f, "quality %d\n",      g_quality);
    fprintf(f, "audio_source %d\n", g_audio_source);
    fprintf(f, "receive_scale %d\n", g_receive_scale);
    fprintf(f, "buttons %d\n",      g_show_buttons);
    fprintf(f, "stats %d\n",        g_show_stats);
    fprintf(f, "pad_colour %d\n",   vpad_colour());
    fprintf(f, "pad_opacity %d\n",  vpad_opacity());
    fprintf(f, "stick_left %d\n",   vpad_stick_below(0));
    fprintf(f, "stick_right %d\n",  vpad_stick_below(1));
    (void)g_pad_colour; (void)g_pad_opacity;
    fclose(f);
}

static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f)
        return;
    char key[32];
    int value;
    while (fscanf(f, "%31s %d", key, &value) == 2) {
        if      (!strcmp(key, "volume"))        g_volume = value;
        else if (!strcmp(key, "muted"))         g_muted = value;
        else if (!strcmp(key, "quality"))       g_quality = value;
        else if (!strcmp(key, "audio_source"))  g_audio_source = value;
        else if (!strcmp(key, "receive_scale")) g_receive_scale = value;
        else if (!strcmp(key, "buttons"))       g_show_buttons = value;
        else if (!strcmp(key, "stats"))         g_show_stats = value;
        else if (!strcmp(key, "pad_colour"))    g_pad_colour = value;
        else if (!strcmp(key, "pad_opacity"))   g_pad_opacity = value;
        else if (!strcmp(key, "stick_left"))    g_stick_l = value;
        else if (!strcmp(key, "stick_right"))   g_stick_r = value;
    }
    fclose(f);

    /* Anything out of range means a file written by something else, or
     * by hand. Fall back rather than index an array with it. */
    if (g_volume < 0 || g_volume > 100) g_volume = 100;
    if (g_quality < 0 ||
        g_quality >= (int)(sizeof(QUALITY) / sizeof(QUALITY[0]))) g_quality = 0;
    if (g_audio_source < 0 || g_audio_source > 2) g_audio_source = 0;
}

static void save_layout(int console)
{
    char path[128];
    snprintf(path, sizeof(path), LAYOUT_PATH_FMT, console);
    mkdir("sdmc:/switch", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    char text[1024];
    vpad_layout_to_string(text, sizeof(text));
    fprintf(f, "%s %s\n", LAYOUT_VERSION, text);
    fclose(f);
}

static void load_layout(int console)
{
    char path[128];
    snprintf(path, sizeof(path), LAYOUT_PATH_FMT, console);
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char text[1024] = "";
    if (fgets(text, sizeof(text), f)) {
        const size_t n = strlen(LAYOUT_VERSION);
        if (strncmp(text, LAYOUT_VERSION, n) == 0 && text[n] == ' ')
            vpad_layout_from_string(text + n + 1);
        /* Anything else was written by a build whose positions meant
         * something different. Left alone rather than deleted: it costs
         * nothing, and the next save replaces it. */
    }
    fclose(f);
}

static void save_server(void)
{
    remember_server();
    mkdir("sdmc:/switch", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    for (int i = 0; i < g_server_count; i++)
        fprintf(f, "%s %u\n", g_servers[i].host, (unsigned)g_servers[i].port);
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
/*
 * How wide a band the on-screen buttons need down each side.
 *
 * A 4:3 screen already leaves exactly this much: 960 wide on a 1280
 * panel is 160 either side, which is where the buttons go for nothing.
 * A Wii U's 16:9 fills the width and leaves none, so there the picture
 * gives some back -- but only while the buttons are actually shown.
 */
#define BUTTON_BAND 160


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
    if (g_show_buttons && SCREEN_W - dw < 2 * BUTTON_BAND) {
        dw = SCREEN_W - 2 * BUTTON_BAND;
        dh = dw * h / w;
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

/*
 * Between the on-screen pad's vocabulary and the protocol's.
 *
 * vpad.c speaks in slots named after an Xbox controller, because that is
 * what it was written against; this protocol speaks in Nintendo button
 * codes. The two meet here and nowhere else, which is why the table is
 * written out rather than the pad being rewritten.
 */
static u64 mask_for_code(int code)
{
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++)
        if (BUTTONS[i].code == code)
            return BUTTONS[i].mask;
    return 0;
}

static const struct { int slot; int code; } SLOT_TO_CODE[] = {
    { PAD_Y,     BS_BTN_X },      /* top    */
    { PAD_X,     BS_BTN_Y },      /* left   */
    { PAD_B,     BS_BTN_A },      /* right  */
    { PAD_A,     BS_BTN_B },      /* bottom */
    { PAD_UP,    BS_BTN_UP },    { PAD_DOWN,  BS_BTN_DOWN },
    { PAD_LEFT,  BS_BTN_LEFT },  { PAD_RIGHT, BS_BTN_RIGHT },
    { PAD_LB,    BS_BTN_L },     { PAD_RB,    BS_BTN_R },
    { PAD_LT,    BS_BTN_ZL },    { PAD_RT,    BS_BTN_ZR },
    { PAD_BACK,  BS_BTN_SELECT },{ PAD_START, BS_BTN_START },
    { PAD_GUIDE, BS_BTN_HOME },
};

/* The console's own controller, in those same slots, so the glass and
 * the Joy-Cons can be merged before either is looked at. */
static void pad_from_joycons(PadState21 out, u64 held,
                             const HidAnalogStickState *l,
                             const HidAnalogStickState *r)
{
    memset(out, 0, sizeof(PadState21));
    for (size_t i = 0; i < sizeof(SLOT_TO_CODE) / sizeof(SLOT_TO_CODE[0]); i++) {
        const u64 mask = mask_for_code(SLOT_TO_CODE[i].code);
        if (mask && (held & mask))
            out[SLOT_TO_CODE[i].slot] = 100;
    }
    out[PAD_LX] = (int8_t)(l->x * 100 / 32767);
    out[PAD_LY] = (int8_t)(l->y * 100 / 32767);
    out[PAD_RX] = (int8_t)(r->x * 100 / 32767);
    out[PAD_RY] = (int8_t)(r->y * 100 / 32767);
}

/* Only what changed, so a held button is not re-sent sixty times a
 * second and a resting stick says nothing at all. */
static void send_pad_state(const PadState21 now)
{
    static PadState21 before;
    static int primed;

    for (size_t i = 0; i < sizeof(SLOT_TO_CODE) / sizeof(SLOT_TO_CODE[0]); i++) {
        const int slot = SLOT_TO_CODE[i].slot;
        const int on = now[slot] != 0, was = primed && before[slot] != 0;
        if (on != was)
            stream_send_button(SLOT_TO_CODE[i].code, on);
    }

    static const struct { int slot; int axis; int flip; } AXES[] = {
        { PAD_LX, BS_AXIS_LEFT_X,  0 }, { PAD_LY, BS_AXIS_LEFT_Y,  0 },
        { PAD_RX, BS_AXIS_RIGHT_X, 0 }, { PAD_RY, BS_AXIS_RIGHT_Y, 0 },
    };
    for (size_t i = 0; i < sizeof(AXES) / sizeof(AXES[0]); i++) {
        const int slot = AXES[i].slot;
        if (primed && now[slot] == before[slot])
            continue;
        int v = now[slot] * 32767 / 100;
        if (v > 32767) v = 32767;
        if (v < -32767) v = -32767;
        stream_send_axis(AXES[i].axis, AXES[i].flip ? -v : v);
    }

    memcpy(before, now, sizeof(PadState21));
    primed = 1;
}


/*
 * The stylus, and only the stylus.
 *
 * The coordinates sent are in the space the server announced, not the
 * console's native size: the stream is larger whenever the emulator
 * renders at a higher internal resolution, and dividing by the native
 * size instead would put every tap wrong by exactly that scale.
 *
 * The on-screen pad reads the glass itself and binds each finger to
 * whatever control it landed on, so a touch it has claimed must not also
 * be a pen stroke. vpad_near_control answers that, and anything it does
 * not want that falls inside the picture is the stylus.
 */
static void send_touch(const SDL_Rect *dst, const StreamInfo *info)
{
    static int touching;

    /*
     * Nothing at all while the top screen is being shown. There is no
     * touch panel behind that picture, so a stroke here is not one that
     * missed -- it is one that should never leave. The finger already
     * down is lifted first, or the emulator would be left holding a
     * stylus that never came up.
     */
    if (g_screen != BS_SCREEN_BOTTOM) {
        if (touching) {
            touching = 0;
            stream_send_touch(BS_INPUT_TOUCH_UP, 0, 0);
        }
        return;
    }

    HidTouchScreenState st = {0};
    const int have = hidGetTouchScreenStates(&st, 1) && st.count > 0;
    int stylus = -1;

    if (have) {
        for (int t = 0; t < (int)st.count; t++) {
            const int tx = (int)st.touches[t].x, ty = (int)st.touches[t].y;
            if (vpad_enabled() &&
                vpad_near_control((float)tx / SCREEN_W, (float)ty / SCREEN_H))
                continue;
            if (tx >= dst->x && tx < dst->x + dst->w &&
                ty >= dst->y && ty < dst->y + dst->h)
                stylus = t;
        }
    }

    if (stylus < 0) {
        if (touching) {
            touching = 0;
            stream_send_touch(BS_INPUT_TOUCH_UP, 0, 0);
        }
        return;
    }

    const int x = ((int)st.touches[stylus].x - dst->x) * info->width / dst->w;
    const int y = ((int)st.touches[stylus].y - dst->y) * info->height / dst->h;
    stream_send_touch(touching ? BS_INPUT_TOUCH_MOVE : BS_INPUT_TOUCH_DOWN, x, y);
    touching = 1;
}

/* -------------------------------------------------------------- sound */

static void audio_callback(void *user, Uint8 *out, int len)
{
    (void)user;
    const int frames = len / (int)(sizeof(int16_t) * 2);
    const int got = stream_take_audio((int16_t *)out, frames);
    /* Applied here rather than in the mixer: there is one place sound
     * leaves this program, and one number is easier to be sure of than
     * a gain scattered through the path. */
    if (g_volume != 100 || g_muted) {
        int16_t *pcm = (int16_t *)out;
        const int scale = g_muted ? 0 : g_volume;
        for (int i = 0; i < got * 2; i++)
            pcm[i] = (int16_t)((int)pcm[i] * scale / 100);
    }
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

        load_layout(info.console);

        /* The server starts on its own defaults, so anything remembered
         * has to be asked for again on every connection or it quietly
         * does nothing after the first one. */
        if (QUALITY[g_quality].bitrate)
            stream_send_quality(QUALITY[g_quality].bitrate);
        if (g_audio_source)
            stream_send_audio_source(g_audio_source);
        /* A new connection lands on the bottom screen whatever the last
         * one showed, so the two are put back in step here rather than
         * left disagreeing about what is on the wire. */
        g_screen = BS_SCREEN_BOTTOM;
        if (g_receive_scale) {
            int nw = 0, nh = 0;
            native_size(info.console, &nw, &nh);
            if (g_receive_scale == -2) stream_send_size(nw / 2, nh / 2);
            else stream_send_size(nw * g_receive_scale, nh * g_receive_scale);
        }
        snprintf(g_message, sizeof(g_message), "%dx%d, %s",
                 info.width, info.height, stream_decoder_name());
    } else {
        snprintf(g_message, sizeof(g_message), "%s", err);
    }
}

static void fill(int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(g_renderer, &rect);
}

/*
 * One row: a translucent band, brighter when it is the one selected,
 * with its label on the left and whatever it is set to on the right.
 */
/*
 * The menu, on one page.
 *
 * It was one column of 66-pixel rows, and it grew past the bottom of the
 * screen -- fifteen of them is 990 pixels on a panel that is 720, so the
 * last third simply was not there and nothing scrolled to reach it.
 *
 * Two columns and shorter rows instead of a scrollbar: everything at
 * once is worth more here than anywhere, because this is read with a
 * thumb on a d-pad rather than a mouse on a wheel. Grouped under
 * headings, the same groups the browser page uses, so the two are
 * learned once.
 */
#define ROW_H      40
#define HEADING_H  32
#define MENU_TOP  156
#define COL_GAP    24

static int row_height(const MenuRow *r)
{
    return r->kind == ROW_HEADING ? HEADING_H : ROW_H;
}

/* Where each row ends up, worked out once and used by both the drawing
 * and nothing else -- the navigation moves by index, not by position. */
static void layout_rows(int *col_of, int *y_of)
{
    int total = 0;
    for (int i = 0; i < g_row_count; i++)
        total += row_height(&g_rows[i]) + 4;

    /* Split so a heading is never left at the foot of a column with its
     * rows in the next one. */
    const int target = total / 2;
    int split = g_row_count, run = 0;
    for (int i = 0; i < g_row_count; i++) {
        run += row_height(&g_rows[i]) + 4;
        if (run >= target && g_rows[i].kind != ROW_HEADING) {
            split = i + 1;
            while (split < g_row_count && g_rows[split].kind == ROW_HEADING)
                break;
            break;
        }
    }

    int y0 = MENU_TOP, y1 = MENU_TOP;
    for (int i = 0; i < g_row_count; i++) {
        const int col = (i < split) ? 0 : 1;
        col_of[i] = col;
        if (col == 0) { y_of[i] = y0; y0 += row_height(&g_rows[i]) + 4; }
        else          { y_of[i] = y1; y1 += row_height(&g_rows[i]) + 4; }
    }
}

static void draw_row(int index, int col, int y)
{
    const MenuRow *row = &g_rows[index];
    const int selected = (index == g_selected);
    const int w = (SCREEN_W - 2 * 70 - COL_GAP) / 2;
    const int x = 70 + col * (w + COL_GAP);

    if (row->kind == ROW_HEADING) {
        draw_text(g_small, row->label, x + 6, y + 8, COL_ACCENT);
        return;
    }

    fill(x, y, w, ROW_H, selected ? COL_SELECTED : COL_ROW);
    draw_text(g_small, row->label, x + 16, y + 8,
              row->kind == ROW_INFO ? COL_DIM : COL_TEXT);

    if (row->value[0]) {
        int tw = 0, th = 0;
        TTF_SizeUTF8(g_small, row->value, &tw, &th);
        draw_text(g_small, row->value, x + w - 16 - tw, y + 8,
                  selected ? COL_TEXT : COL_DIM);
    }
}

static void draw_rows(int top)
{
    (void)top;
    int col_of[24], y_of[24];
    layout_rows(col_of, y_of);
    for (int i = 0; i < g_row_count; i++)
        draw_row(i, col_of[i], y_of[i]);
}

/* The console's own screen, which every offered size is a multiple of. */
/*
 * The screen being watched, at its own native size.
 *
 * Not the console's, which is the bottom screen's and is the wrong
 * answer for a 3DS: its touch screen is 320x240 and its top screen is
 * 400x240, which is 5:3 rather than 4:3. Offering multiples of the
 * bottom screen while the top one is on the wire asks the server to
 * squeeze a 5:3 picture into a 4:3 box, and it obliges. The DS's two
 * screens are the same size and a Wii U's are both 16:9, so the 3DS is
 * the one that shows it.
 */
static void screen_native_size(int console, int screen, int *w, int *h)
{
    if (screen == BS_SCREEN_TOP) {
        switch (console) {
        case BS_CONSOLE_3DS:
            *w = BS_3DS_TOP_WIDTH;  *h = BS_3DS_TOP_HEIGHT;  break;
        case BS_CONSOLE_WIIU:
            *w = BS_WIIU_TOP_WIDTH; *h = BS_WIIU_TOP_HEIGHT; break;
        default:
            *w = BS_DS_TOP_WIDTH;   *h = BS_DS_TOP_HEIGHT;   break;
        }
        return;
    }
    native_size(console, w, h);
}

static void native_size(int console, int *w, int *h)
{
    switch (console) {
    case BS_CONSOLE_3DS:  *w = BS_3DS_WIDTH;  *h = BS_3DS_HEIGHT;  break;
    case BS_CONSOLE_WIIU: *w = BS_WIIU_WIDTH; *h = BS_WIIU_HEIGHT; break;
    default:              *w = BS_DS_WIDTH;   *h = BS_DS_HEIGHT;   break;
    }
}

static void build_connect_rows(void)
{
    g_row_count = 0;
    MenuRow *r;

    r = add_row();
    r->kind = ROW_HEADING; r->label = "server"; r->value[0] = '\0';

    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_ADDRESS; r->label = "address";
    snprintf(r->value, sizeof(r->value), "%s", g_host);

    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_PORT; r->label = "port";
    snprintf(r->value, sizeof(r->value), "%u", (unsigned)g_port);

    /* Only worth a row once there is a choice to make. */
    if (g_server_count > 1) {
        r = add_row();
        r->kind = ROW_VALUE; r->id = ROWID_SAVED; r->label = "saved";
        snprintf(r->value, sizeof(r->value), "%d of %d   %s:%u",
                 g_server_at + 1, g_server_count,
                 g_servers[g_server_at].host,
                 (unsigned)g_servers[g_server_at].port);
    }

    r = add_row();
    r->kind = ROW_ACTION; r->id = ROWID_CONNECT;
    r->label = "connect"; r->value[0] = '\0';

    settle_selection();
}

/*
 * What may be asked for is whole multiples of the console's own screen,
 * never below it -- the Wii U alone may halve, because 854x480 has the
 * room. An arbitrary fraction of a DS screen is smaller than a decoder
 * will produce frames from at all.
 */
static void size_label(const StreamInfo *info, char *out, size_t outlen)
{
    int nw = 0, nh = 0;
    screen_native_size(info->console, g_screen, &nw, &nh);
    if (g_receive_scale == 0)
        snprintf(out, outlen, "as rendered  %dx%d", info->width, info->height);
    else if (g_receive_scale == -2)
        snprintf(out, outlen, "half native  %dx%d", nw / 2, nh / 2);
    else
        snprintf(out, outlen, "%dx native  %dx%d", g_receive_scale,
                 nw * g_receive_scale, nh * g_receive_scale);
}

static RowId selected_id(void)
{
    if (g_selected < 0 || g_selected >= g_row_count)
        return ROWID_NONE;
    return g_rows[g_selected].id;
}

static void build_play_rows(const StreamInfo *info)
{
    g_row_count = 0;
    MenuRow *r;

    r = add_row();
    r->kind = ROW_HEADING; r->label = "stream"; r->value[0] = '\0';

    /*
     * The other screen, offered only where there is one. Against the
     * point of all this, and useful: on a Wii U the television picture
     * is usually the one worth watching.
     */
    if (stream_screens() & (1 << BS_SCREEN_TOP)) {
        r = add_row();
        r->kind = ROW_VALUE; r->id = ROWID_SCREEN; r->label = "screen";
        snprintf(r->value, sizeof(r->value), "%s",
                 g_screen == BS_SCREEN_TOP ? "top (no touch)" : "bottom");
    }

    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_SIZE; r->label = "size received";
    size_label(info, r->value, sizeof(r->value));

    /* The same ladder the other two clients offer, so the same words
     * mean the same thing wherever you are holding it. */
    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_QUALITY; r->label = "quality";
    snprintf(r->value, sizeof(r->value), "%s", QUALITY[g_quality].label);

    r = add_row();
    r->kind = ROW_HEADING; r->label = "on-screen buttons"; r->value[0] = '\0';

    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_BUTTONS; r->label = "shown";
    snprintf(r->value, sizeof(r->value), "%s", g_show_buttons ? "shown" : "hidden");

    /* Only worth offering once there is a pad on screen to arrange. */
    if (g_show_buttons) {
        r = add_row();
        r->kind = ROW_VALUE; r->id = ROWID_PAD_EDIT; r->label = "move buttons";
        snprintf(r->value, sizeof(r->value), "%s",
                 vpad_editing() ? "dragging" : "no");

        r = add_row();
        r->kind = ROW_VALUE; r->id = ROWID_PAD_COLOUR; r->label = "button colour";
        snprintf(r->value, sizeof(r->value), "%s", vpad_colour_name());

        r = add_row();
        r->kind = ROW_VALUE; r->id = ROWID_PAD_OPACITY; r->label = "button opacity";
        snprintf(r->value, sizeof(r->value), "%d%%", vpad_opacity());

        /* Only where there are sticks to place. */
        if (info->console != BS_CONSOLE_DS) {
            r = add_row();
            r->kind = ROW_VALUE; r->id = ROWID_STICK_L; r->label = "left stick";
            snprintf(r->value, sizeof(r->value), "%s",
                     vpad_stick_below(0) ? "below the d-pad" : "above the d-pad");

            r = add_row();
            r->kind = ROW_VALUE; r->id = ROWID_STICK_R; r->label = "right stick";
            snprintf(r->value, sizeof(r->value), "%s",
                     vpad_stick_below(1) ? "below the buttons" : "above the buttons");
        }

        r = add_row();
        r->kind = ROW_ACTION; r->id = ROWID_PAD_RESET;
        r->label = "reset button positions"; r->value[0] = '\0';
    }

    r = add_row();
    r->kind = ROW_HEADING; r->label = "sound"; r->value[0] = '\0';

    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_VOLUME; r->label = "volume";
    snprintf(r->value, sizeof(r->value), "%d%%%s", g_volume,
             g_muted ? "   muted" : "");

    /* Only a Wii U has two outputs, so only there is there a choice to
     * make. Offering it on a DS would be a question with one answer. */
    if (info->console == BS_CONSOLE_WIIU) {
        static const char *const names[3] = { "both, summed", "television",
                                              "GamePad" };
        r = add_row();
        r->kind = ROW_VALUE; r->id = ROWID_AUDIO_SOURCE; r->label = "sound from";
        snprintf(r->value, sizeof(r->value), "%s", names[g_audio_source]);

        /* A Wii U GamePad has a HOME button and a Switch has none to
         * spare, so it lives here rather than being unreachable. */
        r = add_row();
        r->kind = ROW_ACTION; r->id = ROWID_HOME;
        r->label = "press HOME"; r->value[0] = '\0';
    }

    r = add_row();
    r->kind = ROW_HEADING; r->label = "session"; r->value[0] = '\0';

    r = add_row();
    r->kind = ROW_VALUE; r->id = ROWID_STATS; r->label = "counters";
    snprintf(r->value, sizeof(r->value), "%s", g_show_stats ? "shown" : "hidden");

    r = add_row();
    r->kind = ROW_ACTION; r->id = ROWID_DISCONNECT;
    r->label = "disconnect"; r->value[0] = '\0';

    r = add_row();
    r->kind = ROW_INFO; r->id = ROWID_DECODER; r->label = "decoder";
    snprintf(r->value, sizeof(r->value), "%s", stream_decoder_name());

    settle_selection();
}

/*
 * Left and right on the selected row. Sizes step through the multiples
 * the console allows; volume moves in tens because a stick-less menu
 * should not need forty presses to be heard.
 */
static void adjust_row(const StreamInfo *info, int delta)
{
    int nw = 0, nh = 0;
    screen_native_size(info->console, g_screen, &nw, &nh);

    switch (selected_id()) {
    case ROWID_SIZE: {
        /* The order is: half (Wii U only), native, 2x, 3x..., then
         * whatever is rendered. */
        int steps[12], n = 0;
        if (nw >= 640) steps[n++] = -2;
        /* Nothing past the ceiling: the server brings anything taller
         * back down, and an option that quietly means something else is
         * worse than no option at all. */
        for (int f = 1; nw * f <= info->width &&
                        nh * f <= BS_MAX_STREAM_HEIGHT &&
                        n < (int)(sizeof(steps) / sizeof(steps[0])) - 1; f++)
            steps[n++] = f;
        steps[n++] = 0;

        int at = 0;
        for (int i = 0; i < n; i++) if (steps[i] == g_receive_scale) at = i;
        at += delta;
        if (at < 0) at = 0;
        if (at >= n) at = n - 1;
        g_receive_scale = steps[at];

        if (g_receive_scale == 0)      stream_send_size(0, 0);
        else if (g_receive_scale == -2) stream_send_size(nw / 2, nh / 2);
        else stream_send_size(nw * g_receive_scale, nh * g_receive_scale);
        break;
    }
    case ROWID_QUALITY: {
        const int n = (int)(sizeof(QUALITY) / sizeof(QUALITY[0]));
        g_quality += delta;
        if (g_quality < 0) g_quality = 0;
        if (g_quality >= n) g_quality = n - 1;
        stream_send_quality(QUALITY[g_quality].bitrate);
        break;
    }
    case ROWID_SCREEN:
        /*
         * Two of them, so either direction is the other one. The size
         * that comes back is the acknowledgement; nothing here has to
         * wait for it, because the picture changing shape is already
         * handled the way an emulator's internal resolution moving is.
         */
        g_screen = (g_screen == BS_SCREEN_TOP) ? BS_SCREEN_BOTTOM
                                               : BS_SCREEN_TOP;
        stream_send_screen(g_screen);
        /*
         * The screen that has just been joined has its own encoder, and
         * that encoder was built on the server's defaults -- it has
         * never heard of anything chosen here. Both settings are said
         * again, or a picture asked for at a low bitrate arrives at full
         * quality on the other screen and nothing in the menu explains
         * why. The size is re-derived from the screen now on the wire,
         * which for a 3DS is a different shape.
         */
        if (QUALITY[g_quality].bitrate)
            stream_send_quality(QUALITY[g_quality].bitrate);
        {
            int sw = 0, sh = 0;
            screen_native_size(info->console, g_screen, &sw, &sh);
            if (g_receive_scale == 0)       stream_send_size(0, 0);
            else if (g_receive_scale == -2) stream_send_size(sw / 2, sh / 2);
            else stream_send_size(sw * g_receive_scale, sh * g_receive_scale);
        }
        break;
    case ROWID_BUTTONS:
        g_show_buttons = !g_show_buttons;
        break;
    case ROWID_STATS:
        g_show_stats = !g_show_stats;
        break;
    case ROWID_PAD_EDIT:
        vpad_set_editing(!vpad_editing());
        break;
    case ROWID_PAD_COLOUR:
        vpad_set_colour(vpad_colour() + delta);
        break;
    case ROWID_PAD_OPACITY:
        vpad_set_opacity(vpad_opacity() + delta * 10);
        break;
    case ROWID_STICK_L:
        vpad_set_stick_below(0, !vpad_stick_below(0));
        break;
    case ROWID_STICK_R:
        vpad_set_stick_below(1, !vpad_stick_below(1));
        break;
    case ROWID_VOLUME:
        g_volume += delta * 10;
        if (g_volume < 0) g_volume = 0;
        if (g_volume > 100) g_volume = 100;
        g_muted = (g_volume == 0);
        break;
    case ROWID_AUDIO_SOURCE:
        g_audio_source += delta;
        if (g_audio_source < 0) g_audio_source = 0;
        if (g_audio_source > 2) g_audio_source = 2;
        stream_send_audio_source(g_audio_source);
        break;
    default:
        break;
    }
    save_settings();
}

static void draw_menu(void)
{
    SDL_SetRenderDrawColor(g_renderer, 16, 17, 22, 255);
    SDL_RenderClear(g_renderer);

    draw_text(g_font, "Bottom Screen", 90, 60, COL_TEXT);
    draw_rows(150);

    draw_text(g_small, "\u2191\u2193  choose   \u2190\u2192  change   A  use",
              70, SCREEN_H - 44, COL_DIM);
    if (g_message[0])
        draw_text(g_small, g_message, 70, SCREEN_H - 84,
                  COL_BAD);

    SDL_RenderPresent(g_renderer);
}

/*
 * The settings, over the picture, in the same rows as the menu. Opened
 * with Minus and closed with B, because a console's own menus work that
 * way and nothing here should have to be learned twice.
 */
static void draw_play_menu(const StreamInfo *info)
{
    fill(0, 0, SCREEN_W, SCREEN_H, (SDL_Color){ 8, 10, 16, 200 });

    char head[128];
    snprintf(head, sizeof(head), "%dx%d  %d fps  %s",
             info->width, info->height, info->fps,
             info->audio_rate > 0 ? "sound" : "no sound");
    draw_text(g_font, "settings", 90, 60, COL_ACCENT);
    draw_text(g_small, head, 90, 118, COL_GOOD);

    build_play_rows(info);
    draw_rows(180);

    draw_text(g_small,
              "\u2191\u2193  choose   \u2190\u2192 or A  change   B  back",
              70, SCREEN_H - 44, COL_DIM);
}

/* What the two of them together are doing, for the pad to light up. */
static PadState21 g_merged_pad;

/* One bit per vpad anchor: what the machine on the other end has got. */
static unsigned present_for(int console)
{
    unsigned m = (1u << VPAD_DPAD) | (1u << VPAD_FACE) |
                 (1u << VPAD_LB)   | (1u << VPAD_RB) |
                 (1u << VPAD_SELECT) | (1u << VPAD_START);
    if (console == BS_CONSOLE_3DS || console == BS_CONSOLE_WIIU)
        m |= (1u << VPAD_LT) | (1u << VPAD_RT) |
             (1u << VPAD_LSTICK) | (1u << VPAD_RSTICK);
    if (console == BS_CONSOLE_WIIU)
        m |= 1u << VPAD_GUIDE;
    /* No console here has a stick you can click. */
    return m;
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

    if (g_show_buttons)
        vpad_draw(g_renderer, g_merged_pad);

    /*
     * The counters, over everything else and in green.
     *
     * Over, because a line hidden behind the picture is a line you turn
     * on and cannot find; green, because it is a diagnostic and should
     * not be mistaken for part of the game. Off by default: it is
     * wanted while something is wrong, and in the way the rest of the
     * time.
     */
    if (g_show_stats) {
        char probe[128];
        snprintf(probe, sizeof(probe),
                 "%s   %ux%u   %u frames   \u2013   START+SELECT for settings",
                 stream_decoder_name(), (unsigned)info->width,
                 (unsigned)info->height, (unsigned)stream_frames());
        int tw = 0, th = 0;
        TTF_SizeUTF8(g_small, probe, &tw, &th);
        fill(8, 8, tw + 16, th + 8, (SDL_Color){ 0, 0, 0, 170 });
        draw_text(g_small, probe, 16, 12, COL_GOOD);
    }

    if (g_menu_open) {
        draw_play_menu(info);
    } else {
        /*
         * The decoder's name and its output, small and in a corner.
         *
         * A stream that is connected and one that is arriving look
         * identical when both are black, and the difference between the
         * console's own video block and a software fallback is the
         * difference between a cool machine and a hot one.
         */
        /* Already drawn above. */
    }

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
    load_settings();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
        goto done;
    if (TTF_Init() != 0)
        goto done;

    g_window = SDL_CreateWindow("Bottom Screen", 0, 0, SCREEN_W, SCREEN_H, 0);
    /*
     * With vsync, because without it appletMainLoop spins as fast as the
     * hardware allows: thousands of iterations a second, each one asking
     * the pad and the touchscreen and sending whatever changed. On a
     * handheld that is battery burnt for nothing, and under an emulator
     * it starves the thing of the time it needs to present anything at
     * all.
     */
    g_renderer = SDL_CreateRenderer(g_window, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
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
    ui_bind(g_small, g_font);
    vpad_init();
    /* Applied after the pad exists: load_settings runs before it, so
     * anything handed straight to vpad there would be overwritten by
     * its own initialisation. */
    vpad_set_colour(g_pad_colour);
    vpad_set_opacity(g_pad_opacity);
    vpad_set_stick_below(0, g_stick_l);
    vpad_set_stick_below(1, g_stick_r);

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


        if (!stream_connected()) {
            build_connect_rows();

            if (down & HidNpadButton_Down) move_selection(1);
            if (down & HidNpadButton_Up)   move_selection(-1);

            /* Left and right walk the saved list, and choosing one fills
             * the address and port above rather than making you retype
             * them on a console keyboard. */
            if (selected_id() == ROWID_SAVED && g_server_count > 0 &&
                (down & (HidNpadButton_Left | HidNpadButton_Right))) {
                g_server_at += (down & HidNpadButton_Right) ? 1 : -1;
                if (g_server_at < 0) g_server_at = g_server_count - 1;
                if (g_server_at >= g_server_count) g_server_at = 0;
                snprintf(g_host, sizeof(g_host), "%s", g_servers[g_server_at].host);
                g_port = g_servers[g_server_at].port;
            }

            if (down & HidNpadButton_A) {
                char typed[64];
                switch (selected_id()) {
                case ROWID_ADDRESS:
                    if (ask_text("Server address", g_host, typed, sizeof(typed))) {
                        snprintf(g_host, sizeof(g_host), "%s", typed);
                        save_server();
                    }
                    break;
                case ROWID_PORT: {
                    char current[16];
                    snprintf(current, sizeof(current), "%u", (unsigned)g_port);
                    if (ask_text("Port", current, typed, sizeof(typed))) {
                        int p = atoi(typed);
                        if (p > 0 && p < 65536) {
                            g_port = (uint16_t)p;
                            save_server();
                        }
                    }
                    break;
                }
                default:
                    try_connect();
                    break;
                }
            }
            draw_menu();
            continue;
        }

        StreamInfo info;
        stream_info(&info);

        /*
         * START and SELECT together: a second opens the settings, five
         * seconds leave the application.
         *
         * Both are buttons the game wants, so neither can do anything on
         * its own, and a moment's overlap while playing must not count
         * either -- hence the hold. When the menu opens they are
         * released towards the game, which would otherwise be left
         * holding two buttons nobody is pressing any more.
         *
         * While the settings are open the pad drives them rather than
         * the console: a menu that also presses A on the game underneath
         * it is worse than no menu.
         */
        {
            static Uint32 combo_since = 0;
            static int    combo_opened = 0;
            const u64 holding = padGetButtons(&pad);
            const int both = (holding & HidNpadButton_Plus) &&
                             (holding & HidNpadButton_Minus);
            if (!both) {
                combo_since = 0;
                combo_opened = 0;
            } else {
                if (!combo_since)
                    combo_since = SDL_GetTicks();
                const Uint32 held_ms = SDL_GetTicks() - combo_since;
                if (!combo_opened && held_ms >= 1000) {
                    combo_opened = 1;
                    g_menu_open = !g_menu_open;
                    g_selected = 0;
                    stream_send_button(BS_BTN_START, 0);
                    stream_send_button(BS_BTN_SELECT, 0);
                }
                if (held_ms >= 5000)
                    break;
            }
        }

        if (g_menu_open) {
            if (down & HidNpadButton_B) g_menu_open = 0;
            if (down & HidNpadButton_Down) move_selection(1);
            if (down & HidNpadButton_Up)   move_selection(-1);
            if (down & (HidNpadButton_Left | HidNpadButton_Right))
                adjust_row(&info, (down & HidNpadButton_Right) ? 1 : -1);
            if (down & HidNpadButton_A) {
                switch (selected_id()) {
                case ROWID_DISCONNECT:
                    stream_disconnect();
                    g_menu_open = 0;
                    snprintf(g_message, sizeof(g_message), "%s", "");
                    break;
                case ROWID_PAD_RESET:
                    vpad_reset_layout();
                    break;
                case ROWID_HOME:
                    /* Down and up together: there is nothing to hold. */
                    stream_send_button(BS_BTN_HOME, 1);
                    stream_send_button(BS_BTN_HOME, 0);
                    break;
                default:
                    /*
                     * A on anything else means the same as Right.
                     *
                     * The footer says the two are different -- arrows
                     * change a value, A uses an action -- and that is
                     * true of a list of sizes. It is not true of a
                     * two-state option: "screen: bottom" looks like
                     * something you press, and pressing it did nothing
                     * at all, which reads as an option that was never
                     * wired up. It was; it just refused the button
                     * everybody tries first.
                     */
                    adjust_row(&info, 1);
                    break;
                }
            }
            draw_playing(&info, &picture_rect);
            continue;
        }

        /*
         * The console's controller and the glass are read into one
         * state and merged before anything is sent, so a Joy-Con in one
         * hand and a thumb on the screen are a valid way to play rather
         * than two sources fighting.
         */
        vpad_set_enabled(g_show_buttons);
        vpad_set_present(present_for(info.console));
        /*
         * The pad is sized from the band it has to fit in.
         *
         * The widest control is the face cluster, four units across; at
         * full size that is wider than the band itself, which is how it
         * came to spill onto the picture. Deriving the scale means it
         * fits whatever the stream's shape leaves -- narrowest on a Wii
         * U, whose 16:9 gives back the least.
         */
        {
            const int band = picture_rect.x;
            if (band > 0)
                vpad_set_scale((float)(band - 14) / (4.0f * 44.0f));
        }

        /* No control on the picture: on a DS or a 3DS that surface is
         * the stylus, so a button there would swallow taps meant for
         * the game. */
        vpad_set_forbidden(picture_rect);
        {
            /* vpad counts its own changes, so a dragged button is
             * noticed rather than the saving being told about it. */
            static unsigned seen_revision;
            const unsigned now = vpad_layout_revision();
            if (now != seen_revision) {
                seen_revision = now;
                save_layout(info.console);
            }
        }
        vpad_set_accepting(!g_menu_open);
        vpad_poll_touches();

        PadState21 merged;
        HidAnalogStickState left = padGetStickPos(&pad, 0);
        HidAnalogStickState right = padGetStickPos(&pad, 1);
        pad_from_joycons(merged, padGetButtons(&pad), &left, &right);
        vpad_merge(merged);
        send_pad_state(merged);
        memcpy(g_merged_pad, merged, sizeof(PadState21));

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
