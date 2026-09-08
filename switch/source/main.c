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

#define SCREEN_W 1280
#define SCREEN_H 720

#define CONFIG_DIR  "sdmc:/switch/bottom_screen"
#define CONFIG_PATH   CONFIG_DIR "/server.txt"
#define SETTINGS_PATH CONFIG_DIR "/settings.txt"

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
typedef enum { ROW_ACTION, ROW_VALUE, ROW_INFO } RowKind;

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
    ROWID_STATS
} RowId;

typedef struct {
    RowKind kind;
    RowId   id;
    const char *label;
    char value[64];
} MenuRow;

/* The row the cursor is on, by identity. */
static RowId selected_id(void);

static MenuRow g_rows[12];
static int     g_row_count;
static int     g_selected;

/* Settings while playing, reached by holding START and SELECT. */
static int g_menu_open;
static int g_receive_scale;      /* 0 = as rendered, N = N x native */
static int g_volume = 100;
static int g_muted;
static int g_show_buttons;       /* off until asked for */
static int g_show_stats;         /* likewise: wanted when something is wrong */
/* BS_AUDIO_BOTH until somebody says otherwise. */
static int g_audio_source;

/* The ladder the web and Android clients offer, in the same words. */
static const struct { const char *label; int bitrate; } QUALITY[] = {
    { "automatic",         0 },
    { "low  ~400 kbit/s",  400000 },
    { "medium  ~1 Mbit/s", 1000000 },
    { "high  ~2.5 Mbit/s", 2500000 },
    { "maximum  ~6 Mbit/s", 6000000 },
};
static int g_quality;

static void native_size(int console, int *w, int *h);

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
    }
    fclose(f);

    /* Anything out of range means a file written by something else, or
     * by hand. Fall back rather than index an array with it. */
    if (g_volume < 0 || g_volume > 100) g_volume = 100;
    if (g_quality < 0 ||
        g_quality >= (int)(sizeof(QUALITY) / sizeof(QUALITY[0]))) g_quality = 0;
    if (g_audio_source < 0 || g_audio_source > 2) g_audio_source = 0;
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

static void send_pad(u64 down, u64 up)
{
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        if (down & BUTTONS[i].mask) stream_send_button(BUTTONS[i].code, 1);
        if (up   & BUTTONS[i].mask) stream_send_button(BUTTONS[i].code, 0);
    }
}

/*
 * Below this a stick counts as centred. A Joy-Con does not return to
 * exactly zero, and forwarding that residue walks the character slowly
 * into a wall while nobody is touching anything -- the web client uses
 * the same 12%, and this had none at all.
 */
#define STICK_DEADZONE 3900

static int deadzoned(int v)
{
    return (v > -STICK_DEADZONE && v < STICK_DEADZONE) ? 0 : v;
}

static void send_sticks(const HidAnalogStickState *left,
                        const HidAnalogStickState *right)
{
    static int lx, ly, rx, ry;
    const int nlx = deadzoned(left->x),  nly = deadzoned(left->y);
    const int nrx = deadzoned(right->x), nry = deadzoned(right->y);
    /* Only on change: a stick at rest would otherwise fill the link with
     * events saying nothing happened. */
    if (nlx != lx) { lx = nlx; stream_send_axis(BS_AXIS_LEFT_X, lx); }
    if (nly != ly) { ly = nly; stream_send_axis(BS_AXIS_LEFT_Y, ly); }
    if (nrx != rx) { rx = nrx; stream_send_axis(BS_AXIS_RIGHT_X, rx); }
    if (nry != ry) { ry = nry; stream_send_axis(BS_AXIS_RIGHT_Y, ry); }
}

/*
 * A finger on the picture becomes a stylus on the console.
 *
 * The coordinates sent are in the space the server announced, not the
 * console's native size: the stream is larger whenever the emulator
 * renders at a higher internal resolution, and dividing by the native
 * size instead would put every tap wrong by exactly that scale.
 */
/* ------------------------------------------------ on-screen buttons */

static void fill(int x, int y, int w, int h, SDL_Color c);

/*
 * A Switch has every button a DS or a 3DS has, so these are not there to
 * make the console playable: they are for the ones it has not got -- a
 * Wii U's HOME -- and for playing with the tablet held in two hands and
 * the Joy-Cons off.
 *
 * Which is what decides where they go. Held that way, the thumbs reach
 * the lower corners and nothing else, so the controls that get used sit
 * low and the triggers, which are pressed rarely, sit out of the way at
 * the top. They live in the black either side of the picture, which on a
 * 4:3 stream costs nothing at all.
 *
 * Each console gets what it actually has: no ZL on a DS, no stick on
 * either handheld that has not got one.
 */
typedef struct {
    SDL_Rect rect;
    int      code;
    const char *label;
} PadButton;

typedef struct {
    int cx, cy, radius;
    int axis_x, axis_y;
    const char *label;
    int touch;              /* a finger is on it */
    int kx, ky;             /* where the knob is drawn */
} PadStick;

#define MAX_PAD_BUTTONS 20
static PadButton g_pad_buttons[MAX_PAD_BUTTONS];
static int       g_pad_button_count;
static PadStick  g_pad_sticks[2];
static int       g_pad_stick_count;
/* Which buttons a finger is on, one bit each. */
static uint32_t  g_pad_touched;
/* What the Joy-Cons are holding, so the drawing can show it. */
static u64       g_pad_held;

static void add_pad_button(int cx, int y, int w, int h, int code, const char *label)
{
    if (g_pad_button_count >= MAX_PAD_BUTTONS)
        return;
    PadButton *b = &g_pad_buttons[g_pad_button_count++];
    b->rect = (SDL_Rect){ cx - w / 2, y, w, h };
    b->code = code;
    b->label = label;
}

static void add_pad_stick(int cx, int cy, int r, int ax, int ay, const char *label)
{
    if (g_pad_stick_count >= 2)
        return;
    PadStick *st = &g_pad_sticks[g_pad_stick_count++];
    st->cx = cx; st->cy = cy; st->radius = r;
    st->axis_x = ax; st->axis_y = ay;
    st->label = label;
    st->touch = 0;
    st->kx = cx; st->ky = cy;
}

static void layout_pad_buttons(const StreamInfo *info, const SDL_Rect *pic)
{
    g_pad_button_count = 0;
    g_pad_stick_count = 0;
    if (!g_show_buttons)
        return;

    const int lw = pic->x;                   /* left band  */
    const int rx = pic->x + pic->w;
    const int rw = SCREEN_W - rx;            /* right band */
    if (lw < 110 || rw < 110)
        return;                              /* no room: draw none */

    const int lcx = lw / 2, rcx = rx + rw / 2;
    const int band = (lw < rw ? lw : rw);
    const int wide_w = band - 28, wide_h = 34;
    const int u = 44, gap = 12;

    const int wiiu = (info->console == BS_CONSOLE_WIIU);
    const int n3ds = (info->console == BS_CONSOLE_3DS);
    const int has_z     = wiiu || n3ds;
    const int has_stick = wiiu || n3ds;

    /* Built from the bottom, because that is where the thumbs are. */
    int lb = SCREEN_H - 24 - wide_h;
    int rb = lb;
    add_pad_button(lcx, lb, wide_w, wide_h, BS_BTN_SELECT, "SELECT");
    add_pad_button(rcx, rb, wide_w, wide_h, BS_BTN_START, "START");
    if (wiiu) {
        lb -= wide_h + gap;
        add_pad_button(lcx, lb, wide_w, wide_h, BS_BTN_HOME, "HOME");
    }

    int lmid, rmid;
    if (has_stick) {
        const int r = (band - 40) / 2;
        const int sy = lb - gap - r;
        add_pad_stick(lcx, sy, r, BS_AXIS_LEFT_X, BS_AXIS_LEFT_Y,
                      n3ds ? "\u25CB" : "L");
        add_pad_stick(rcx, sy, r, BS_AXIS_RIGHT_X, BS_AXIS_RIGHT_Y,
                      n3ds ? "C" : "R");
        lmid = rmid = sy - r - gap - u * 3 / 2;
    } else {
        lmid = rmid = lb - gap - u * 3 / 2;
    }

    /* D-pad on the left, face buttons on the right. */
    add_pad_button(lcx,         lmid - u,     u, u, BS_BTN_UP,    "\u25B2");
    add_pad_button(lcx - u,     lmid,         u, u, BS_BTN_LEFT,  "\u25C0");
    add_pad_button(lcx + u,     lmid,         u, u, BS_BTN_RIGHT, "\u25B6");
    add_pad_button(lcx,         lmid + u,     u, u, BS_BTN_DOWN,  "\u25BC");

    add_pad_button(rcx,         rmid - u,     u, u, BS_BTN_X, "X");
    add_pad_button(rcx - u,     rmid,         u, u, BS_BTN_Y, "Y");
    add_pad_button(rcx + u,     rmid,         u, u, BS_BTN_A, "A");
    add_pad_button(rcx,         rmid + u,     u, u, BS_BTN_B, "B");

    /* Triggers at the top, where they are out of the way of a thumb
     * resting in the corner, with Z above its shoulder as on the
     * machine. */
    int ly = 40, ry = 40;
    if (has_z) {
        add_pad_button(lcx, ly, wide_w, wide_h, BS_BTN_ZL, "ZL"); ly += wide_h + gap;
        add_pad_button(rcx, ry, wide_w, wide_h, BS_BTN_ZR, "ZR"); ry += wide_h + gap;
    }
    add_pad_button(lcx, ly, wide_w, wide_h, BS_BTN_L, "L");
    add_pad_button(rcx, ry, wide_w, wide_h, BS_BTN_R, "R");
}

/* The Joy-Con mask that means the same thing as a button code, so a
 * press on the console lights the button on the screen. */
static u64 mask_for_code(int code)
{
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++)
        if (BUTTONS[i].code == code)
            return BUTTONS[i].mask;
    return 0;
}

static void draw_centred(const char *s, int cx, int cy, SDL_Color colour)
{
    int tw = 0, th = 0;
    TTF_SizeUTF8(g_small, s, &tw, &th);
    draw_text(g_small, s, cx - tw / 2, cy - th / 2, colour);
}

static void draw_pad_buttons(u64 held)
{
    for (int i = 0; i < g_pad_button_count; i++) {
        const PadButton *b = &g_pad_buttons[i];
        const int on = (g_pad_touched & (1u << i)) ||
                       (held & mask_for_code(b->code));
        fill(b->rect.x, b->rect.y, b->rect.w, b->rect.h,
             on ? COL_SELECTED : COL_ROW);
        draw_centred(b->label, b->rect.x + b->rect.w / 2,
                     b->rect.y + b->rect.h / 2, on ? COL_TEXT : COL_DIM);
    }

    for (int i = 0; i < g_pad_stick_count; i++) {
        const PadStick *st = &g_pad_sticks[i];
        const int r = st->radius;
        fill(st->cx - r, st->cy - r, r * 2, r * 2, COL_ROW);
        const int kr = r / 2;
        fill(st->kx - kr, st->ky - kr, kr * 2, kr * 2,
             st->touch ? COL_SELECTED : COL_ROW);
        draw_centred(st->label, st->cx, st->cy - r - 12, COL_DIM);
    }
}

static void send_touch(const SDL_Rect *dst, const StreamInfo *info)
{
    static int touching;

    HidTouchScreenState st = {0};
    const int have = hidGetTouchScreenStates(&st, 1) && st.count > 0;

    const uint32_t was = g_pad_touched;
    g_pad_touched = 0;
    int stylus = -1;

    int stick_was[2];
    for (int i = 0; i < g_pad_stick_count; i++) {
        stick_was[i] = g_pad_sticks[i].touch;
        g_pad_sticks[i].touch = 0;
    }

    if (have) {
        for (int t = 0; t < (int)st.count; t++) {
            const int tx = (int)st.touches[t].x, ty = (int)st.touches[t].y;

            int claimed = 0;
            for (int i = 0; i < g_pad_button_count; i++) {
                const SDL_Rect *r = &g_pad_buttons[i].rect;
                if (tx >= r->x && tx < r->x + r->w &&
                    ty >= r->y && ty < r->y + r->h) {
                    g_pad_touched |= 1u << i;
                    claimed = 1;
                    break;
                }
            }
            if (claimed)
                continue;

            /* A stick keeps the finger that started on it even when it
             * is dragged past the ring, which is how a thumb actually
             * moves -- releasing at the edge would be a stick that lets
             * go on its own. */
            for (int i = 0; i < g_pad_stick_count; i++) {
                PadStick *st = &g_pad_sticks[i];
                const int dx = tx - st->cx, dy = ty - st->cy;
                const int reach = st->touch ? st->radius * 2 : st->radius;
                if (dx * dx + dy * dy > reach * reach)
                    continue;
                st->touch = 1;
                float fx = (float)dx / st->radius;
                float fy = (float)dy / st->radius;
                const float m = sqrtf(fx * fx + fy * fy);
                if (m > 1.0f) { fx /= m; fy /= m; }
                st->kx = st->cx + (int)(fx * st->radius * 0.6f);
                st->ky = st->cy + (int)(fy * st->radius * 0.6f);
                stream_send_axis(st->axis_x, (int)(fx * 32767.0f));
                /* Screen y grows downwards, sticks report up as
                 * positive. */
                stream_send_axis(st->axis_y, (int)(-fy * 32767.0f));
                claimed = 1;
                break;
            }
            if (claimed)
                continue;

            if (tx >= dst->x && tx < dst->x + dst->w &&
                ty >= dst->y && ty < dst->y + dst->h)
                stylus = t;
        }
    }

    /* A stick nobody is holding any more goes back to centre: one left
     * off-centre keeps walking after the thumb has gone. */
    for (int i = 0; i < g_pad_stick_count; i++) {
        PadStick *st = &g_pad_sticks[i];
        if (stick_was[i] && !st->touch) {
            st->kx = st->cx; st->ky = st->cy;
            stream_send_axis(st->axis_x, 0);
            stream_send_axis(st->axis_y, 0);
        }
    }

    /* Only the changes, so a held button is not re-sent sixty times a
     * second. */
    for (int i = 0; i < g_pad_button_count; i++) {
        const uint32_t bit = 1u << i;
        if ((g_pad_touched & bit) && !(was & bit))
            stream_send_button(g_pad_buttons[i].code, 1);
        else if (!(g_pad_touched & bit) && (was & bit))
            stream_send_button(g_pad_buttons[i].code, 0);
    }

    if (stylus < 0) {
        /* Outside the picture: not a stylus press, and treating it as
         * one would put the pen on the far edge of the screen. */
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

        /* The server starts on its own defaults, so anything remembered
         * has to be asked for again on every connection or it quietly
         * does nothing after the first one. */
        if (QUALITY[g_quality].bitrate)
            stream_send_quality(QUALITY[g_quality].bitrate);
        if (g_audio_source)
            stream_send_audio_source(g_audio_source);
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
static void draw_row(int index, int y)
{
    const MenuRow *row = &g_rows[index];
    const int selected = (index == g_selected);
    const int x = 90, w = SCREEN_W - 180, h = 56;

    fill(x, y, w, h, selected ? COL_SELECTED : COL_ROW);
    draw_text(g_small, row->label, x + 24, y + 14,
              row->kind == ROW_INFO ? COL_DIM : COL_TEXT);

    if (row->value[0]) {
        int tw = 0, th = 0;
        TTF_SizeUTF8(g_small, row->value, &tw, &th);
        draw_text(g_small, row->value, x + w - 24 - tw, y + 14,
                  selected ? COL_TEXT : COL_DIM);
    }
}

static void draw_rows(int top)
{
    for (int i = 0; i < g_row_count; i++)
        draw_row(i, top + i * 66);
}

/* The console's own screen, which every offered size is a multiple of. */
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

    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_ADDRESS; r->label = "address";
    snprintf(r->value, sizeof(r->value), "%s", g_host);

    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_PORT; r->label = "port";
    snprintf(r->value, sizeof(r->value), "%u", (unsigned)g_port);

    /* Only worth a row once there is a choice to make. */
    if (g_server_count > 1) {
        r = &g_rows[g_row_count++];
        r->kind = ROW_VALUE; r->id = ROWID_SAVED; r->label = "saved";
        snprintf(r->value, sizeof(r->value), "%d of %d   %s:%u",
                 g_server_at + 1, g_server_count,
                 g_servers[g_server_at].host,
                 (unsigned)g_servers[g_server_at].port);
    }

    r = &g_rows[g_row_count++];
    r->kind = ROW_ACTION; r->id = ROWID_CONNECT;
    r->label = "connect"; r->value[0] = '\0';

    if (g_selected >= g_row_count)
        g_selected = g_row_count - 1;
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
    native_size(info->console, &nw, &nh);
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

    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_SIZE; r->label = "size received";
    size_label(info, r->value, sizeof(r->value));

    /* The same ladder the other two clients offer, so the same words
     * mean the same thing wherever you are holding it. */
    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_QUALITY; r->label = "quality";
    snprintf(r->value, sizeof(r->value), "%s", QUALITY[g_quality].label);

    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_BUTTONS; r->label = "on-screen buttons";
    snprintf(r->value, sizeof(r->value), "%s", g_show_buttons ? "shown" : "hidden");

    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_STATS; r->label = "counters";
    snprintf(r->value, sizeof(r->value), "%s", g_show_stats ? "shown" : "hidden");

    r = &g_rows[g_row_count++];
    r->kind = ROW_VALUE; r->id = ROWID_VOLUME; r->label = "volume";
    snprintf(r->value, sizeof(r->value), "%d%%%s", g_volume,
             g_muted ? "   muted" : "");

    /* Only a Wii U has two outputs, so only there is there a choice to
     * make. Offering it on a DS would be a question with one answer. */
    if (info->console == BS_CONSOLE_WIIU) {
        static const char *const names[3] = { "both, summed", "television",
                                              "GamePad" };
        r = &g_rows[g_row_count++];
        r->kind = ROW_VALUE; r->id = ROWID_AUDIO_SOURCE; r->label = "sound from";
        snprintf(r->value, sizeof(r->value), "%s", names[g_audio_source]);

        /* A Wii U GamePad has a HOME button and a Switch has none to
         * spare, so it lives here rather than being unreachable. */
        r = &g_rows[g_row_count++];
        r->kind = ROW_ACTION; r->id = ROWID_HOME;
        r->label = "press HOME"; r->value[0] = '\0';
    }

    r = &g_rows[g_row_count++];
    r->kind = ROW_ACTION; r->id = ROWID_DISCONNECT;
    r->label = "disconnect"; r->value[0] = '\0';

    r = &g_rows[g_row_count++];
    r->kind = ROW_INFO; r->id = ROWID_DECODER; r->label = "decoder";
    snprintf(r->value, sizeof(r->value), "%s", stream_decoder_name());

    if (g_selected >= g_row_count)
        g_selected = g_row_count - 1;
}

/*
 * Left and right on the selected row. Sizes step through the multiples
 * the console allows; volume moves in tens because a stick-less menu
 * should not need forty presses to be heard.
 */
static void adjust_row(const StreamInfo *info, int delta)
{
    int nw = 0, nh = 0;
    native_size(info->console, &nw, &nh);

    switch (selected_id()) {
    case ROWID_SIZE: {
        /* The order is: half (Wii U only), native, 2x, 3x..., then
         * whatever is rendered. */
        int steps[8], n = 0;
        if (nw >= 640) steps[n++] = -2;
        for (int f = 1; nw * f <= info->width; f++) steps[n++] = f;
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
    case ROWID_BUTTONS:
        g_show_buttons = !g_show_buttons;
        break;
    case ROWID_STATS:
        g_show_stats = !g_show_stats;
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

    draw_text(g_small, "\u2191\u2193  choose   \u2190\u2192  change   A  use   +  quit",
              90, 150 + g_row_count * 66 + 24, COL_DIM);
    if (g_message[0])
        draw_text(g_small, g_message, 90, 150 + g_row_count * 66 + 70,
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
              "\u2191\u2193  choose      \u2190\u2192  change      A  use      B  back",
              90, 180 + g_row_count * 66 + 24, COL_DIM);
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
            layout_pad_buttons(info, &dst);
            draw_pad_buttons(g_pad_held);
        }
    }

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


        if (!stream_connected()) {
            build_connect_rows();

            if (down & HidNpadButton_Down)
                g_selected = (g_selected + 1) % g_row_count;
            if (down & HidNpadButton_Up)
                g_selected = (g_selected + g_row_count - 1) % g_row_count;

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
            if (down & HidNpadButton_Down)
                g_selected = (g_selected + 1) % g_row_count;
            if (down & HidNpadButton_Up)
                g_selected = (g_selected + g_row_count - 1) % g_row_count;
            if (down & (HidNpadButton_Left | HidNpadButton_Right))
                adjust_row(&info, (down & HidNpadButton_Right) ? 1 : -1);
            if (down & HidNpadButton_A) {
                switch (selected_id()) {
                case ROWID_DISCONNECT:
                    stream_disconnect();
                    g_menu_open = 0;
                    snprintf(g_message, sizeof(g_message), "%s", "");
                    break;
                case ROWID_HOME:
                    /* Down and up together: there is nothing to hold. */
                    stream_send_button(BS_BTN_HOME, 1);
                    stream_send_button(BS_BTN_HOME, 0);
                    break;
                default:
                    break;
                }
            }
            draw_playing(&info, &picture_rect);
            continue;
        }

        g_pad_held = padGetButtons(&pad);
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
