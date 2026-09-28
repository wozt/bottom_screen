/*
 * Native Bottom Screen client running on the Wii U console itself.
 *
 * Protocol semantics come from bottom_screen's Switch client.
 * Hardware paths come from capture2cloud's validated Wii U client:
 *
 *   TCP -> H264DEC -> NV12 -> GX2
 *   Opus -> native PCM s16 -> AX
 *   Wii U GamePad -> BsInputEvent
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <whb/log.h>
#include <whb/log_udp.h>

#include "audio.h"
#include "bs_protocol.h"
#include "input.h"
#include "keyboard.h"
#include "proc.h"
#include "settings.h"
#include "stream.h"
#include "ui.h"
#include "video.h"
#include "video_worker.h"

#define MAX_WIDTH       1280
#define MAX_HEIGHT       720
#define VIDEO_AU_CAP (1024 * 1024)

typedef struct {
    int x, y, w, h;
} Rect;

typedef enum {
    MENU_CONNECTION = 0,
    MENU_STREAM,
    MENU_AUDIO,
    MENU_DIAGNOSTICS,
    MENU_PAGE_COUNT
} MenuPage;

typedef struct {
    int active;
    int page;
    BsPrompt prompt;
    char body[BS_PROMPT_MAX];
} PromptUi;

static const Rect R_TABS[MENU_PAGE_COUNT] = {
    { 190, 112, 210, 46 },
    { 410, 112, 210, 46 },
    { 630, 112, 210, 46 },
    { 850, 112, 210, 46 },
};
static const Rect R_CLOSE      = { 880, 620, 180, 46 };
static const Rect R_MARKER     = { 1260, 0, 20, 20 };

static const struct {
    const char *label;
    int bitrate;
} QUALITY[] = {
    { "Automatic", 0 },
    { "Low  400 kbit/s", 400000 },
    { "Medium  1 Mbit/s", 1000000 },
    { "High  2.5 Mbit/s", 2500000 },
    { "Maximum  6 Mbit/s", 6000000 },
};

static Rect menu_row(int row)
{
    Rect r = { 220, 178 + row * 64, 840, 54 };
    return r;
}

static Rect prompt_choice_row(int slot)
{
    Rect r = { 220, 164 + slot * 58, 840, 48 };
    return r;
}

static const char *prompt_part(const PromptUi *ui, int part)
{
    const char *p = ui->body;
    size_t left = sizeof(ui->body);

    for (int i = 0; i <= part; ++i) {
        if (!left)
            return NULL;
        size_t n = 0;
        while (n < left && p[n])
            n++;
        if (n >= left)
            return NULL;
        if (i == part)
            return p;
        p += n + 1;
        left -= n + 1;
    }
    return NULL;
}

static int prompt_choice_count(const PromptUi *ui)
{
    int count = 0;
    for (int i = 0; i < ui->prompt.choices; ++i) {
        if (!prompt_part(ui, i + 1))
            break;
        count++;
    }
    return count;
}

static int hit(const Rect *r, int x, int y)
{
    return x >= r->x && y >= r->y &&
           x < r->x + r->w &&
           y < r->y + r->h;
}

static int supports_remote_home(int console)
{
    return console == BS_CONSOLE_3DS ||
           console == BS_CONSOLE_WIIU;
}

static void draw_button(Rect r,
                        const char *text,
                        UiColour fill)
{
    ui_box(r.x, r.y, r.w, r.h, fill, UI_DIM);
    ui_text_centred(
        r.x, r.y, r.w, r.h,
        UI_SIZE_BODY,
        UI_TEXT,
        text);
}

static void fit_rect(int src_w,
                     int src_h,
                     Rect *out)
{
    out->x = 0;
    out->y = 0;
    out->w = UI_WIDTH;
    out->h = UI_HEIGHT;

    if (src_w <= 0 || src_h <= 0)
        return;

    if ((long long)src_w * UI_HEIGHT >
        (long long)src_h * UI_WIDTH) {

        out->h =
            (int)((long long)UI_WIDTH *
                  src_h / src_w);

        out->y =
            (UI_HEIGHT - out->h) / 2;

    } else {
        out->w =
            (int)((long long)UI_HEIGHT *
                  src_w / src_h);

        out->x =
            (UI_WIDTH - out->w) / 2;
    }
}

static int rebuild_decoder(int *video_alive,
                           int *worker_alive,
                           char *why,
                           size_t why_size)
{
    if (*worker_alive) {
        video_worker_stop();
        *worker_alive = 0;
    }

    if (*video_alive) {
        video_exit();
        *video_alive = 0;
    }

    if (video_init(
            MAX_WIDTH,
            MAX_HEIGHT,
            why,
            why_size) != 0) {
        return -1;
    }

    *video_alive = 1;

    if (video_worker_start(
            why,
            why_size) != 0) {

        video_exit();
        *video_alive = 0;
        return -1;
    }

    *worker_alive = 1;
    return 0;
}

static void draw_value_row(int row,
                           const char *label,
                           const char *value,
                           int enabled)
{
    const Rect r = menu_row(row);
    ui_box(r.x, r.y, r.w, r.h,
           enabled ? UI_FIELD : UI_PANEL,
           UI_DIM);
    ui_text(r.x + 18, r.y + 5, UI_SIZE_BODY,
            UI_DIM, "%s", label);
    ui_text(r.x + 330, r.y + 5, UI_SIZE_BODY,
            enabled ? UI_TEXT : UI_DIM, "%s", value);
}

static void draw_split_row(int row,
                           const char *left,
                           UiColour left_colour,
                           const char *right,
                           UiColour right_colour)
{
    const Rect r = menu_row(row);
    const Rect a = { r.x, r.y, (r.w - 12) / 2, r.h };
    const Rect b = { a.x + a.w + 12, r.y, a.w, r.h };
    draw_button(a, left, left_colour);
    draw_button(b, right, right_colour);
}

static void native_size(int console, int screen, int *w, int *h)
{
    if (console == BS_CONSOLE_WIIU) {
        *w = screen == BS_SCREEN_TOP ? BS_WIIU_TOP_WIDTH : BS_WIIU_WIDTH;
        *h = screen == BS_SCREEN_TOP ? BS_WIIU_TOP_HEIGHT : BS_WIIU_HEIGHT;
    } else if (console == BS_CONSOLE_3DS) {
        *w = screen == BS_SCREEN_TOP ? BS_3DS_TOP_WIDTH : BS_3DS_WIDTH;
        *h = screen == BS_SCREEN_TOP ? BS_3DS_TOP_HEIGHT : BS_3DS_HEIGHT;
    } else {
        *w = BS_DS_WIDTH;
        *h = BS_DS_HEIGHT;
    }
}

static int scale_fits_decoder(int console, int screen, int scale)
{
    if (scale == 0)
        return 1;
    int w = 0, h = 0;
    native_size(console, screen, &w, &h);
    if (scale == -2)
        return w >= 640;
    return w * scale <= MAX_WIDTH &&
           h * scale <= MAX_HEIGHT &&
           h * scale <= BS_MAX_STREAM_HEIGHT;
}

static void normalise_scales(Settings *settings, int console)
{
    for (int screen = 0; screen < BS_SCREEN_COUNT; ++screen) {
        if (!scale_fits_decoder(console, screen,
                                settings->receive_scale[screen]))
            settings->receive_scale[screen] = 1;
    }
}

static void size_label(int scale,
                       int console,
                       int screen,
                       char *out,
                       size_t out_size)
{
    if (scale == 0) {
        snprintf(out, out_size, "Whatever is rendered");
        return;
    }

    int w = 0, h = 0;
    native_size(console, screen, &w, &h);
    if (scale == -2) {
        snprintf(out, out_size, "Half native  %dx%d", w / 2, h / 2);
    } else {
        snprintf(out, out_size, "%dx native  %dx%d",
                 scale, w * scale, h * scale);
    }
}

static void draw_menu(const Settings *settings,
                      const char *note,
                      int connected,
                      int decoder_ok,
                      MenuPage page,
                      int actual_fps,
                      int audio_alive)
{
    static const char *const tabs[MENU_PAGE_COUNT] = {
        "CONNECTION", "STREAM", "AUDIO", "DIAGNOSTICS"
    };
    char value[128];
    StreamInfo info;
    memset(&info, 0, sizeof(info));
    int remote_home_available = 0;

    if (connected) {
        stream_info(&info);
        remote_home_available =
            supports_remote_home(info.console);
    }

    ui_box(
        150, 40, 980, 650,
        ((UiColour){ 0x16, 0x1c, 0x26, 0xf0 }),
        UI_DIM);

    ui_text(
        190, 54,
        UI_SIZE_TITLE,
        UI_TEXT,
        "Bottom Screen");

    ui_text(
        520, 66,
        UI_SIZE_BODY,
        UI_DIM,
        connected ? "Connected" : "Not connected");

    for (int i = 0; i < MENU_PAGE_COUNT; ++i)
        draw_button(R_TABS[i], tabs[i],
                    (MenuPage)i == page ? UI_ACCENT : UI_PANEL);

    if (page == MENU_CONNECTION) {
        draw_value_row(0, "Host name / IPv4", settings->host,
                       !connected);
        snprintf(value, sizeof(value), "%u", settings->port);
        draw_value_row(1, "Port", value, !connected);

        if (settings->server_count > 0) {
            const SavedServer *srv =
                &settings->servers[settings->selected_server];
            snprintf(value, sizeof(value), "%u/%u  %s:%u",
                     settings->selected_server + 1,
                     settings->server_count,
                     srv->host, srv->port);
        } else {
            snprintf(value, sizeof(value), "No saved server");
        }
        draw_value_row(2, "Saved servers  < tap >", value, !connected);
        draw_value_row(3, "Automatic reconnect",
                       settings->auto_connect ? "On" : "Off", 1);
        draw_split_row(4, "SAVE CURRENT", UI_ACCENT,
                       "REMOVE SAVED",
                       settings->server_count && !connected ? UI_DANGER : UI_PANEL);
        draw_split_row(5,
                       connected ? "DISCONNECT" : "CONNECT",
                       connected ? UI_DANGER : UI_ACCENT,
                       "REMOTE HOME",
                       remote_home_available ? UI_ACCENT : UI_FIELD);
    } else if (page == MENU_STREAM) {
        draw_value_row(0, "Screen  (L3+R3)",
                       settings->screen == BS_SCREEN_TOP ? "Top" : "Bottom",
                       !connected || (stream_screens() & (1u << BS_SCREEN_TOP)));

        draw_value_row(1, "Bottom quality  < tap >",
                       QUALITY[settings->quality[BS_SCREEN_BOTTOM]].label, 1);
        size_label(settings->receive_scale[BS_SCREEN_BOTTOM],
                   connected ? info.console : BS_CONSOLE_WIIU,
                   BS_SCREEN_BOTTOM, value, sizeof(value));
        draw_value_row(2, "Bottom size  < tap >", value, 1);

        const int top_available = !connected ||
            (stream_screens() & (1u << BS_SCREEN_TOP));
        draw_value_row(3, "Top quality  < tap >",
                       QUALITY[settings->quality[BS_SCREEN_TOP]].label,
                       top_available);
        size_label(settings->receive_scale[BS_SCREEN_TOP],
                   connected ? info.console : BS_CONSOLE_WIIU,
                   BS_SCREEN_TOP, value, sizeof(value));
        draw_value_row(4, "Top size  < tap >", value, top_available);
    } else if (page == MENU_AUDIO) {
        snprintf(value, sizeof(value), "%u%%", settings->volume);
        draw_value_row(0, "Volume  < -  tap  + >", value, 1);
        draw_value_row(1, "Mute", settings->muted ? "On" : "Off", 1);
        static const char *const sources[] = {
            "TV + GamePad", "Television", "GamePad"
        };
        draw_value_row(2, "Remote Wii U sound from  < tap >",
                       sources[settings->audio_source],
                       !connected || info.console == BS_CONSOLE_WIIU);
        draw_value_row(3, "Playback",
                       audio_alive ? "48 kHz stereo" : "No active audio", 0);
    } else {
        snprintf(value, sizeof(value), "%s:%u",
                 settings->host, settings->port);
        draw_value_row(0, "Server", value, 0);

        if (connected) {
            snprintf(value, sizeof(value), "%dx%d  %d fps target / %d actual",
                     info.width, info.height, info.fps, actual_fps);
        } else {
            snprintf(value, sizeof(value), "Disconnected");
        }
        draw_value_row(1, "Picture", value, 0);

        VideoStats vs;
        VideoWorkerStats ws;
        video_stats_ex(&vs);
        video_worker_stats(&ws);
        snprintf(value, sizeof(value), "%u received  %u decoded  %u dropped",
                 stream_frames(), vs.decoded, ws.dropped);
        draw_value_row(2, "Frames", value, 0);

        snprintf(value, sizeof(value), "Wii U H264DEC  avg %u us  errors %u",
                 vs.decode_avg_us, vs.errors + ws.errors);
        draw_value_row(3, "Decoder", value, decoder_ok);

        unsigned long packets = 0, failed = 0, dropped = 0;
        AudioDiag ad;
        memset(&ad, 0, sizeof(ad));
        if (audio_alive) {
            audio_stats(&packets, &failed, &dropped);
            audio_diag(&ad);
        }
        snprintf(value, sizeof(value), "%lu packets  %lu lost  %u underruns  %ums queued",
                 packets, failed + dropped, ad.underruns,
                 audio_alive ? audio_queue_ms() : 0);
        draw_value_row(4, "Audio", value, 0);

        snprintf(value, sizeof(value), "Bottom %d   Top %d",
                 stream_watching(BS_SCREEN_BOTTOM),
                 stream_watching(BS_SCREEN_TOP));
        draw_value_row(5, "Spectators", value, 0);
    }

    draw_button(
        R_CLOSE,
        connected ? "CLOSE MENU" : "HOME -> Quitter",
        UI_PANEL);

    if (note && note[0]) {
        ui_text(
            220, 650,
            UI_SIZE_BODY,
            decoder_ok ? UI_TEXT : UI_DANGER,
            "%s",
            note);
    } else {
        ui_text(220, 650, UI_SIZE_BODY, UI_DIM,
                "Touch the top-right marker to reopen settings.");
    }
}

static void draw_prompt(const PromptUi *prompt)
{
    const char *title = prompt_part(prompt, 0);
    const int count = prompt_choice_count(prompt);
    const int per_page = 7;
    const int pages = count > 0 ? (count + per_page - 1) / per_page : 1;
    const int first = prompt->page * per_page;

    ui_box(170, 54, 940, 626,
           ((UiColour){ 0x16, 0x1c, 0x26, 0xf8 }), UI_DIM);
    ui_text(220, 78, UI_SIZE_TITLE, UI_TEXT, "%s",
            title && title[0] ? title : "The console is asking");
    ui_text(220, 124, UI_SIZE_BODY, UI_DIM,
            "Choose an answer");

    for (int slot = 0; slot < per_page; ++slot) {
        const int choice = first + slot;
        if (choice >= count)
            break;
        const Rect row = prompt_choice_row(slot);
        draw_button(row, prompt_part(prompt, choice + 1), UI_FIELD);
    }

    Rect prev = { 220, 590, 180, 50 };
    Rect next = { 420, 590, 180, 50 };
    Rect cancel = { 880, 590, 180, 50 };
    draw_button(prev, "PREVIOUS", prompt->page > 0 ? UI_PANEL : UI_FIELD);
    draw_button(next, "NEXT", prompt->page + 1 < pages ? UI_PANEL : UI_FIELD);
    draw_button(cancel, "CANCEL", UI_DANGER);

    ui_text(640, 602, UI_SIZE_BODY, UI_DIM,
            "Page %d/%d", prompt->page + 1, pages);
}

static int reopen_ui(int *input_alive,
                     char *why,
                     size_t why_size)
{
    if (*input_alive) {
        input_exit();
        *input_alive = 0;
    }

    ui_shutdown();

    if (ui_init(why, why_size) != 0)
        return -1;

    char input_why[96] = {0};

    *input_alive =
        input_init(
            input_why,
            sizeof(input_why)) == 0;

    WHBLogPrintf(
        "UI rebuilt after keyboard; input=%s",
        *input_alive ? "ready" : input_why);

    return 0;
}

static void edit_host(Settings *settings,
                      int *input_alive,
                      char *note,
                      size_t note_size)
{
    char current[SETTINGS_HOST_MAX];
    char typed[64];
    char why[96];

    settings_host_string(
        settings,
        current,
        sizeof(current));

    const int r =
        keyboard_prompt(
            "Bottom Screen host",
            current,
            0,
            typed,
            sizeof(typed),
            why,
            sizeof(why));

    if (r < 0) {
        snprintf(note, note_size, "%s", why);
    } else if (r == 1) {
        if (settings_set_host_string(
                settings,
                typed) != 0) {
            snprintf(
                note,
                note_size,
                "Invalid host name: %s",
                typed);
        } else {
            note[0] = '\0';
        }
    }

    why[0] = '\0';

    if (reopen_ui(
            input_alive,
            why,
            sizeof(why)) != 0) {
        snprintf(
            note,
            note_size,
            "UI rebuild: %s",
            why);
    }
}

static void edit_port(Settings *settings,
                      int *input_alive,
                      char *note,
                      size_t note_size)
{
    char current[16];
    char typed[32];
    char why[96];

    snprintf(
        current,
        sizeof(current),
        "%u",
        settings->port);

    const int r =
        keyboard_prompt(
            "Bottom Screen port",
            current,
            1,
            typed,
            sizeof(typed),
            why,
            sizeof(why));

    if (r < 0) {
        snprintf(note, note_size, "%s", why);
    } else if (r == 1) {
        const int port = atoi(typed);

        if (port <= 0 || port > 65535) {
            snprintf(
                note,
                note_size,
                "Invalid port: %s",
                typed);
        } else {
            settings->port =
                (uint16_t)port;

            note[0] = '\0';
        }
    }

    why[0] = '\0';

    if (reopen_ui(
            input_alive,
            why,
            sizeof(why)) != 0) {
        snprintf(
            note,
            note_size,
            "UI rebuild: %s",
            why);
    }
}

static void release_remote_touch(int *touching)
{
    if (*touching) {
        stream_send_touch(
            BS_INPUT_TOUCH_UP,
            0, 0);

        *touching = 0;
    }
}

static int take_remote_prompt(PromptUi *ui,
                              int *input_alive,
                              int *remote_touching,
                              char *note,
                              size_t note_size)
{
    BsPrompt prompt;
    char body[BS_PROMPT_MAX];
    memset(&prompt, 0, sizeof(prompt));
    memset(body, 0, sizeof(body));

    if (!stream_take_prompt_event(&prompt, body, sizeof(body)))
        return 0;

    if (prompt.id == 0) {
        ui->active = 0;
        return 1;
    }

    release_remote_touch(remote_touching);
    input_update(0);

    if (prompt.kind == BS_PROMPT_TEXT) {
        char answer[512];
        char why[96] = {0};
        size_t answer_size = sizeof(answer);
        if (prompt.max_len > 0 &&
            (size_t)prompt.max_len + 1 < answer_size)
            answer_size = (size_t)prompt.max_len + 1;

        const int result = keyboard_prompt(
            body[0] ? body : "The console is asking for text",
            "", 0, answer, answer_size, why, sizeof(why));

        if (result == 1) {
            stream_send_prompt_reply(prompt.id, 0, 0, answer);
        } else {
            stream_send_prompt_reply(prompt.id, 1, 0, "");
            if (result < 0)
                snprintf(note, note_size, "Prompt keyboard: %s", why);
        }

        why[0] = '\0';
        if (reopen_ui(input_alive, why, sizeof(why)) != 0)
            snprintf(note, note_size, "UI rebuild: %s", why);
        return 1;
    }

    if (prompt.kind == BS_PROMPT_CHOICE && prompt.choices > 0) {
        memset(ui, 0, sizeof(*ui));
        ui->active = 1;
        ui->prompt = prompt;
        memcpy(ui->body, body, sizeof(ui->body));
        if (prompt_choice_count(ui) > 0)
            return 1;
        ui->active = 0;
    }

    stream_send_prompt_reply(prompt.id, 1, 0, "");
    return 1;
}

static int tap_remote_prompt(PromptUi *ui, int x, int y)
{
    if (!ui->active)
        return 0;

    const int per_page = 7;
    const int count = prompt_choice_count(ui);
    const int pages = count > 0 ? (count + per_page - 1) / per_page : 1;
    const int first = ui->page * per_page;

    for (int slot = 0; slot < per_page; ++slot) {
        const int choice = first + slot;
        if (choice >= count)
            break;
        Rect row = prompt_choice_row(slot);
        if (hit(&row, x, y)) {
            const char *label = prompt_part(ui, choice + 1);
            stream_send_prompt_reply(ui->prompt.id, 0, choice,
                                     label ? label : "");
            ui->active = 0;
            return 1;
        }
    }

    Rect prev = { 220, 590, 180, 50 };
    Rect next = { 420, 590, 180, 50 };
    Rect cancel = { 880, 590, 180, 50 };
    if (hit(&prev, x, y) && ui->page > 0) {
        ui->page--;
    } else if (hit(&next, x, y) && ui->page + 1 < pages) {
        ui->page++;
    } else if (hit(&cancel, x, y)) {
        stream_send_prompt_reply(ui->prompt.id, 1, 0, "");
        ui->active = 0;
    }
    return 1;
}

static Rect menu_half(int row, int right)
{
    const Rect r = menu_row(row);
    const int w = (r.w - 12) / 2;
    Rect out = { right ? r.x + w + 12 : r.x, r.y, w, r.h };
    return out;
}

static void save_settings_quiet(Settings *settings,
                                char *note,
                                size_t note_size)
{
    char why[96] = {0};
    if (settings_save(settings, why, sizeof(why)) != 0 && note)
        snprintf(note, note_size, "%s", why);
}

static void apply_stream_preferences(const Settings *settings,
                                     int screen,
                                     int console)
{
    if (!stream_connected() || screen < 0 || screen >= BS_SCREEN_COUNT)
        return;

    const int q = settings->quality[screen] < 5
        ? settings->quality[screen]
        : 0;
    stream_send_quality(QUALITY[q].bitrate);

    int w = 0, h = 0;
    native_size(console, screen, &w, &h);
    const int scale = settings->receive_scale[screen];
    if (scale == 0) {
        stream_send_size(0, 0);
    } else if (scale == -2) {
        stream_send_size(w / 2, h / 2);
    } else {
        stream_send_size(w * scale, h * scale);
    }
}

static void press_remote_home(char *note, size_t note_size)
{
    if (!stream_connected()) {
        snprintf(note, note_size, "Connect to a 3DS or Wii U first");
        return;
    }

    StreamInfo info;
    stream_info(&info);
    if (!supports_remote_home(info.console)) {
        snprintf(note, note_size, "Remote HOME needs a 3DS or Wii U");
        return;
    }

    stream_send_button(BS_BTN_HOME, 1);
    SDL_Delay(100);
    stream_send_button(BS_BTN_HOME, 0);
    snprintf(note, note_size, "Remote HOME pressed");
}

static void disconnect_client(int *audio_alive,
                              int *remote_touching,
                              char *note,
                              size_t note_size)
{
    release_remote_touch(remote_touching);
    input_update(0);
    stream_disconnect();
    if (*audio_alive) {
        audio_exit();
        *audio_alive = 0;
    }
    snprintf(note, note_size, "Disconnected");
}

static int connect_client(Settings *settings,
                          int *video_alive,
                          int *worker_alive,
                          int *audio_alive,
                          int *have_frame,
                          int *need_keyframe,
                          char *note,
                          size_t note_size)
{
    char why[160] = {0};

    if (!*video_alive || !*worker_alive) {
        snprintf(note, note_size, "H264DEC is not ready");
        return -1;
    }

    if (rebuild_decoder(video_alive, worker_alive,
                        why, sizeof(why)) != 0) {
        snprintf(note, note_size, "Decoder: %s", why);
        return -1;
    }

    if (stream_connect(settings->host, settings->port,
                       why, sizeof(why)) != 0) {
        snprintf(note, note_size, "Connect: %s", why);
        return -1;
    }

    StreamInfo info;
    stream_info(&info);
    WHBLogPrintf("connected console=%d %dx%d @ %d",
                 info.console, info.width, info.height, info.fps);

    normalise_scales(settings, info.console);

    if (settings->screen != BS_SCREEN_BOTTOM)
        stream_send_screen(settings->screen);
    apply_stream_preferences(settings, settings->screen, info.console);

    if (info.console == BS_CONSOLE_WIIU)
        stream_send_audio_source(settings->audio_source);

    audio_set_volume(settings->volume, settings->muted);
    if (info.audio_rate > 0) {
        why[0] = '\0';
        if (audio_init(48000, 2, why, sizeof(why)) == 0) {
            *audio_alive = 1;
        } else {
            WHBLogPrintf("audio disabled: %s", why);
        }
    }

    settings_remember_server(settings);
    settings_save(settings, why, sizeof(why));
    note[0] = '\0';
    *have_frame = 0;
    *need_keyframe = 0;
    return 0;
}

static void cycle_quality(Settings *settings,
                          int screen,
                          char *note,
                          size_t note_size)
{
    settings->quality[screen] =
        (uint8_t)((settings->quality[screen] + 1) % 5);
    if (stream_connected() && settings->screen == screen)
        stream_send_quality(QUALITY[settings->quality[screen]].bitrate);
    save_settings_quiet(settings, note, note_size);
}

static void cycle_size(Settings *settings,
                       int screen,
                       int console,
                       char *note,
                       size_t note_size)
{
    int steps[6];
    int count = 0;
    int nw = 0, nh = 0;
    native_size(console, screen, &nw, &nh);
    if (nw >= 640)
        steps[count++] = -2;
    for (int scale = 1; scale <= 4; ++scale) {
        if (scale_fits_decoder(console, screen, scale))
            steps[count++] = scale;
    }
    steps[count++] = 0;

    int at = 0;
    for (int i = 0; i < count; ++i) {
        if (steps[i] == settings->receive_scale[screen]) {
            at = i;
            break;
        }
    }
    settings->receive_scale[screen] = (int8_t)steps[(at + 1) % count];
    if (stream_connected() && settings->screen == screen)
        apply_stream_preferences(settings, screen, console);
    save_settings_quiet(settings, note, note_size);
}

static int change_screen(Settings *settings,
                         int screen,
                         int *video_alive,
                         int *worker_alive,
                         int *have_frame,
                         int *need_keyframe,
                         int *remote_touching,
                         char *note,
                         size_t note_size)
{
    if (screen < 0 ||
        screen >= BS_SCREEN_COUNT ||
        screen == settings->screen) {
        return 0;
    }

    if (stream_connected() &&
        !(stream_screens() & (1u << screen))) {

        snprintf(
            note,
            note_size,
            "%s screen is not available",
            screen == BS_SCREEN_TOP ? "Top" : "Bottom");

        return -1;
    }

    release_remote_touch(
        remote_touching);

    if (stream_connected()) {
        char why[160] = {0};

        /*
         * A different screen is a different H.264 stream, even if both
         * happen to be the same size.  Tear H264DEC down before asking the
         * server to send the new SPS/keyframe.  This is the same clean stream
         * boundary used by Capture2Cloud when its H.264 profile changes.
         */
        *have_frame = 0;
        *need_keyframe = 1;

        if (rebuild_decoder(
                video_alive,
                worker_alive,
                why,
                sizeof(why)) != 0) {

            snprintf(
                note,
                note_size,
                "Decoder switch: %s",
                why);

            return -1;
        }

        stream_send_screen(screen);

        StreamInfo info;
        stream_info(&info);
        apply_stream_preferences(settings, screen, info.console);
    }

    settings->screen = (uint8_t)screen;

    char save_why[96] = {0};
    if (settings_save(
            settings,
            save_why,
            sizeof(save_why)) != 0) {

        snprintf(
            note,
            note_size,
            "%s",
            save_why);
    } else {
        snprintf(
            note,
            note_size,
            "%s screen",
            screen == BS_SCREEN_TOP ? "Top" : "Bottom");
    }

    WHBLogPrintf(
        "screen: switched to %s",
        screen == BS_SCREEN_TOP ? "top" : "bottom");

    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogUdpInit();
    WHBLogPrintf("bottom_screen Wii U: starting");

    proc_init();

    Settings settings;
    settings_load(&settings);
    audio_set_volume(settings.volume, settings.muted);

    char why[160] = {0};
    char note[192] = {0};

    if (ui_init(
            why,
            sizeof(why)) != 0) {

        WHBLogPrintf(
            "UI init failed: %s",
            why);

        proc_shutdown();
        WHBLogUdpDeinit();
        return 1;
    }

    int ui_alive = 1;

    char input_why[96] = {0};

    int input_alive =
        input_init(
            input_why,
            sizeof(input_why)) == 0;

    WHBLogPrintf(
        "input: %s",
        input_alive
            ? "GamePad ready"
            : input_why);

    int video_alive = 0;
    int worker_alive = 0;

    const int decoder_ok =
        rebuild_decoder(
            &video_alive,
            &worker_alive,
            why,
            sizeof(why)) == 0;

    if (!decoder_ok) {
        snprintf(
            note,
            sizeof(note),
            "Decoder: %s",
            why);

        WHBLogPrintf(
            "decoder init failed: %s",
            why);
    }

    int audio_alive = 0;
    int menu_open = 1;
    MenuPage menu_page = MENU_CONNECTION;
    int have_frame = 0;
    int remote_touching = 0;
    int last_touch_x = -1;
    int last_touch_y = -1;
    int need_keyframe = 0;
    int auto_reconnect_suspended =
        !settings.auto_connect || strcmp(settings.host, "0.0.0.0") == 0;
    int was_connected = 0;
    uint32_t next_reconnect_at = 0;
    uint32_t fps_started_at = SDL_GetTicks();
    unsigned fps_frames = 0;
    int actual_fps = 0;
    PromptUi prompt_ui;
    memset(&prompt_ui, 0, sizeof(prompt_ui));

    UiInput in;
    memset(&in, 0, sizeof(in));

    VideoFrame frame;
    memset(&frame, 0, sizeof(frame));

    uint8_t *au =
        malloc(VIDEO_AU_CAP);

    if (!au) {
        snprintf(
            note,
            sizeof(note),
            "Out of memory for H.264 buffer");
    }

    int16_t pcm[320 * 2];

    if (settings.auto_connect &&
        strcmp(settings.host, "0.0.0.0") != 0 &&
        connect_client(&settings, &video_alive, &worker_alive,
                       &audio_alive, &have_frame, &need_keyframe,
                       note, sizeof(note)) == 0) {
        menu_open = 0;
        was_connected = 1;
    } else {
        next_reconnect_at = SDL_GetTicks() + 3000;
    }

    while (proc_running()) {

        /*
         * HOME lifecycle: release H264DEC/GX2/AX before ProcUI gives the
         * foreground away, matching the path already validated in
         * Capture2Cloud.
         */
        if (proc_release_pending()) {
            WHBLogPrintf("suspend 1/4: network begin");

            release_remote_touch(
                &remote_touching);

            stream_disconnect();
            prompt_ui.active = 0;

            WHBLogPrintf("suspend 1/4: network done");
            WHBLogPrintf("suspend 2/4: audio begin");

            if (audio_alive) {
                audio_exit();
                audio_alive = 0;
            }

            WHBLogPrintf("suspend 2/4: audio done");
            WHBLogPrintf("suspend 3/4: H264DEC begin");

            if (worker_alive) {
                video_worker_stop();
                worker_alive = 0;
            }

            if (video_alive) {
                video_exit();
                video_alive = 0;
            }

            WHBLogPrintf("suspend 3/4: H264DEC done");
            WHBLogPrintf("suspend 4/4: SDL/GX2 begin");

            if (input_alive) {
                input_exit();
                input_alive = 0;
            }

            if (ui_alive) {
                ui_shutdown();
                ui_alive = 0;
            }

            WHBLogPrintf("suspend 4/4: SDL/GX2 done");

            if (!proc_release_and_wait())
                break;

            why[0] = '\0';

            if (ui_init(
                    why,
                    sizeof(why)) != 0) {
                WHBLogPrintf(
                    "resume UI failed: %s",
                    why);
                break;
            }

            ui_alive = 1;

            input_why[0] = '\0';

            input_alive =
                input_init(
                    input_why,
                    sizeof(input_why)) == 0;

            why[0] = '\0';

            if (rebuild_decoder(
                    &video_alive,
                    &worker_alive,
                    why,
                    sizeof(why)) != 0) {

                snprintf(
                    note,
                    sizeof(note),
                    "Decoder after HOME: %s",
                    why);
            }

            menu_open = 1;
            have_frame = 0;
            memset(&in, 0, sizeof(in));

            continue;
        }

        ui_poll(&in);

        if (in.quit) {
            WHBLogPrintf("ui: quit requested");
            proc_stop();
            continue;
        }

        /*
         * The reader owns the socket. If it died, join it here and make
         * the failure visible instead of keeping the last frame frozen.
         */
        if (was_connected && !stream_connected()) {

            release_remote_touch(
                &remote_touching);

            if (audio_alive) {
                audio_exit();
                audio_alive = 0;
            }

            snprintf(
                note,
                sizeof(note),
                "%s",
                stream_last_error()[0]
                    ? stream_last_error()
                    : "Disconnected");

            stream_disconnect();
            menu_open = 1;
            prompt_ui.active = 0;
            next_reconnect_at = SDL_GetTicks() + 3000;
        }

        was_connected = stream_connected();

        if (!stream_connected() && settings.auto_connect &&
            !auto_reconnect_suspended &&
            (int32_t)(SDL_GetTicks() - next_reconnect_at) >= 0) {
            if (connect_client(&settings, &video_alive, &worker_alive,
                               &audio_alive, &have_frame, &need_keyframe,
                               note, sizeof(note)) == 0) {
                was_connected = 1;
                menu_open = 0;
            } else {
                next_reconnect_at = SDL_GetTicks() + 3000;
            }
        }

        if (stream_connected()) {
            int rw, rh;

            if (stream_take_resize(
                    &rw,
                    &rh)) {

                WHBLogPrintf(
                    "stream resize: %dx%d",
                    rw, rh);

                why[0] = '\0';

                if (rebuild_decoder(
                        &video_alive,
                        &worker_alive,
                        why,
                        sizeof(why)) != 0) {

                    snprintf(
                        note,
                        sizeof(note),
                        "Decoder resize: %s",
                        why);

                } else {
                    have_frame = 0;
                    need_keyframe = 1;
                }
            }

            /*
             * Move a bounded number per UI pass. The network reader
             * already has its own bounded queue; draining forever here
             * would starve presentation and input.
             */
            for (int i = 0; i < 4; ++i) {
                uint32_t au_size = 0;
                int keyframe = 0;

                if (!au ||
                    !stream_take_video(
                        au,
                        VIDEO_AU_CAP,
                        &au_size,
                        &keyframe))
                    break;

                if (need_keyframe &&
                    !keyframe)
                    continue;

                const int submitted =
                    video_worker_submit_wait(
                        au,
                        au_size,
                        12000);

                if (submitted == 0) {
                    need_keyframe = 1;

                    WHBLogPrintf(
                        "video: worker overload, waiting for IDR");
                } else if (submitted > 0 &&
                           keyframe) {
                    need_keyframe = 0;
                }
            }

            if (video_worker_take_wait(
                    &frame,
                    1200)) {

                if (ui_video_update_nv12(
                        frame.luma,
                        frame.chroma,
                        frame.stride,
                        frame.width,
                        frame.height) == 0) {
                    have_frame = 1;
                    fps_frames++;
                }
            }

            const uint32_t fps_now = SDL_GetTicks();
            const uint32_t fps_elapsed = fps_now - fps_started_at;
            if (fps_elapsed >= 1000) {
                actual_fps = (int)(fps_frames * 1000u / fps_elapsed);
                fps_frames = 0;
                fps_started_at = fps_now;
            }

            if (audio_alive) {
                for (;;) {
                    const int frames =
                        stream_take_audio(
                            pcm,
                            320);

                    if (frames <= 0)
                        break;

                    audio_push_pcm_s16_native(
                        pcm,
                        (uint32_t)frames);
                }
            }

            if (take_remote_prompt(&prompt_ui, &input_alive,
                                   &remote_touching, note, sizeof(note)))
                memset(&in, 0, sizeof(in));
        }

        /* Settings/menu touch. */
        if (in.tapped && prompt_ui.active) {
            tap_remote_prompt(&prompt_ui, in.touch_x, in.touch_y);
        } else if (in.tapped) {
            if (!menu_open &&
                hit(&R_MARKER,
                    in.touch_x,
                    in.touch_y)) {

                release_remote_touch(
                    &remote_touching);

                menu_open = 1;

            } else if (menu_open) {
                int changed_page = 0;
                for (int i = 0; i < MENU_PAGE_COUNT; ++i) {
                    if (hit(&R_TABS[i], in.touch_x, in.touch_y)) {
                        menu_page = (MenuPage)i;
                        note[0] = '\0';
                        changed_page = 1;
                    }
                }

                if (changed_page) {
                    /* The same tap must not also activate a row. */
                } else if (menu_page == MENU_CONNECTION) {
                    Rect row0 = menu_row(0);
                    Rect row1 = menu_row(1);
                    Rect row2 = menu_row(2);
                    Rect row3 = menu_row(3);
                    Rect save = menu_half(4, 0);
                    Rect remove = menu_half(4, 1);
                    Rect connect = menu_half(5, 0);
                    Rect home = menu_half(5, 1);

                    if (hit(&row0, in.touch_x, in.touch_y)) {
                        if (stream_connected()) {
                            snprintf(note, sizeof(note),
                                     "Disconnect before changing host");
                        } else {
                            edit_host(&settings, &input_alive,
                                      note, sizeof(note));
                            memset(&in, 0, sizeof(in));
                        }
                    } else if (hit(&row1, in.touch_x, in.touch_y)) {
                        if (stream_connected()) {
                            snprintf(note, sizeof(note),
                                     "Disconnect before changing port");
                        } else {
                            edit_port(&settings, &input_alive,
                                      note, sizeof(note));
                            memset(&in, 0, sizeof(in));
                        }
                    } else if (hit(&row2, in.touch_x, in.touch_y) &&
                               !stream_connected() && settings.server_count) {
                        settings_select_server(
                            &settings,
                            (settings.selected_server + 1) % settings.server_count);
                        save_settings_quiet(&settings, note, sizeof(note));
                    } else if (hit(&row3, in.touch_x, in.touch_y)) {
                        settings.auto_connect = !settings.auto_connect;
                        auto_reconnect_suspended = !settings.auto_connect;
                        save_settings_quiet(&settings, note, sizeof(note));
                    } else if (hit(&save, in.touch_x, in.touch_y)) {
                        const int saved = settings_remember_server(&settings);
                        if (saved < 0) {
                            snprintf(note, sizeof(note), "Server list is full");
                        } else {
                            snprintf(note, sizeof(note), "Server %d saved", saved + 1);
                            save_settings_quiet(&settings, note, sizeof(note));
                        }
                    } else if (hit(&remove, in.touch_x, in.touch_y) &&
                               settings.server_count && !stream_connected()) {
                        settings_remove_server(&settings, settings.selected_server);
                        snprintf(note, sizeof(note), "Saved server removed");
                        save_settings_quiet(&settings, note, sizeof(note));
                    } else if (hit(&connect, in.touch_x, in.touch_y)) {
                        if (stream_connected()) {
                            disconnect_client(&audio_alive, &remote_touching,
                                              note, sizeof(note));
                            prompt_ui.active = 0;
                            auto_reconnect_suspended = 1;
                            was_connected = 0;
                        } else if (connect_client(
                                       &settings, &video_alive, &worker_alive,
                                       &audio_alive, &have_frame, &need_keyframe,
                                       note, sizeof(note)) == 0) {
                            auto_reconnect_suspended = 0;
                            was_connected = 1;
                            menu_open = 0;
                        }
                    } else if (hit(&home, in.touch_x, in.touch_y)) {
                        press_remote_home(note, sizeof(note));
                    }
                } else if (menu_page == MENU_STREAM) {
                    StreamInfo info = {0};
                    if (stream_connected())
                        stream_info(&info);
                    const int console = stream_connected()
                        ? info.console : BS_CONSOLE_WIIU;
                    Rect row0 = menu_row(0);
                    Rect row1 = menu_row(1);
                    Rect row2 = menu_row(2);
                    Rect row3 = menu_row(3);
                    Rect row4 = menu_row(4);

                    if (hit(&row0, in.touch_x, in.touch_y)) {
                        change_screen(&settings,
                                      settings.screen == BS_SCREEN_BOTTOM
                                          ? BS_SCREEN_TOP : BS_SCREEN_BOTTOM,
                                      &video_alive, &worker_alive, &have_frame,
                                      &need_keyframe, &remote_touching,
                                      note, sizeof(note));
                    } else if (hit(&row1, in.touch_x, in.touch_y)) {
                        cycle_quality(&settings, BS_SCREEN_BOTTOM,
                                      note, sizeof(note));
                    } else if (hit(&row2, in.touch_x, in.touch_y)) {
                        cycle_size(&settings, BS_SCREEN_BOTTOM, console,
                                   note, sizeof(note));
                    } else if (hit(&row3, in.touch_x, in.touch_y) &&
                               (!stream_connected() ||
                                (stream_screens() & (1u << BS_SCREEN_TOP)))) {
                        cycle_quality(&settings, BS_SCREEN_TOP,
                                      note, sizeof(note));
                    } else if (hit(&row4, in.touch_x, in.touch_y) &&
                               (!stream_connected() ||
                                (stream_screens() & (1u << BS_SCREEN_TOP)))) {
                        cycle_size(&settings, BS_SCREEN_TOP, console,
                                   note, sizeof(note));
                    }
                } else if (menu_page == MENU_AUDIO) {
                    Rect row0 = menu_row(0);
                    Rect row1 = menu_row(1);
                    Rect row2 = menu_row(2);
                    if (hit(&row0, in.touch_x, in.touch_y)) {
                        int volume = settings.volume;
                        volume += in.touch_x < row0.x + row0.w / 2 ? -10 : 10;
                        if (volume < 0) volume = 0;
                        if (volume > 100) volume = 100;
                        settings.volume = (uint8_t)volume;
                        audio_set_volume(settings.volume, settings.muted);
                        save_settings_quiet(&settings, note, sizeof(note));
                    } else if (hit(&row1, in.touch_x, in.touch_y)) {
                        settings.muted = !settings.muted;
                        audio_set_volume(settings.volume, settings.muted);
                        save_settings_quiet(&settings, note, sizeof(note));
                    } else if (hit(&row2, in.touch_x, in.touch_y)) {
                        StreamInfo info = {0};
                        if (stream_connected()) stream_info(&info);
                        if (!stream_connected() || info.console == BS_CONSOLE_WIIU) {
                            settings.audio_source =
                                (uint8_t)((settings.audio_source + 1) % 3);
                            if (stream_connected())
                                stream_send_audio_source(settings.audio_source);
                            save_settings_quiet(&settings, note, sizeof(note));
                        }
                    }
                }

                if (!changed_page && hit(&R_CLOSE,
                               in.touch_x,
                               in.touch_y) &&
                           stream_connected()) {

                    menu_open = 0;
                }
            }
        }

        /*
         * Physical GamePad controls.
         *
         * Opening the local menu immediately sends a neutral state so a
         * held A/stick cannot remain held remotely.
         */
        input_update(
            stream_connected() &&
            !menu_open &&
            !prompt_ui.active);

        if (input_take_screen_toggle()) {
            change_screen(
                &settings,
                settings.screen == BS_SCREEN_BOTTOM
                    ? BS_SCREEN_TOP
                    : BS_SCREEN_BOTTOM,
                &video_alive,
                &worker_alive,
                &have_frame,
                &need_keyframe,
                &remote_touching,
                note,
                sizeof(note));
        }

        /*
         * Real Wii U GamePad touchscreen -> emulated bottom screen.
         *
         * SDL reports the panel in the same 1280x720 coordinates as the
         * window. Convert from the letterboxed picture rectangle into
         * the server-announced stream coordinate space exactly once.
         */
        if (stream_connected() &&
            !menu_open &&
            !prompt_ui.active &&
            settings.screen == BS_SCREEN_BOTTOM) {

            StreamInfo info;
            stream_info(&info);

            Rect picture;
            fit_rect(
                info.width,
                info.height,
                &picture);

            const int inside =
                in.touch_x >= picture.x &&
                in.touch_y >= picture.y &&
                in.touch_x < picture.x + picture.w &&
                in.touch_y < picture.y + picture.h;

            if (in.touching &&
                inside &&
                picture.w > 0 &&
                picture.h > 0) {

                const int tx =
                    (in.touch_x - picture.x) *
                    info.width /
                    picture.w;

                const int ty =
                    (in.touch_y - picture.y) *
                    info.height /
                    picture.h;

                if (!remote_touching) {
                    stream_send_touch(
                        BS_INPUT_TOUCH_DOWN,
                        tx, ty);

                    remote_touching = 1;
                    last_touch_x = tx;
                    last_touch_y = ty;

                } else if (tx != last_touch_x ||
                           ty != last_touch_y) {

                    stream_send_touch(
                        BS_INPUT_TOUCH_MOVE,
                        tx, ty);

                    last_touch_x = tx;
                    last_touch_y = ty;
                }

            } else if (remote_touching) {
                release_remote_touch(
                    &remote_touching);
            }

        } else {
            release_remote_touch(
                &remote_touching);
        }

        ui_begin();

        if (have_frame)
            ui_video_draw();

        if (prompt_ui.active) {
            draw_prompt(&prompt_ui);
        } else if (menu_open ||
            !stream_connected()) {

            draw_menu(
                &settings,
                note,
                stream_connected(),
                decoder_ok,
                menu_page,
                actual_fps,
                audio_alive);

        } else {
            /*
             * Tiny visible menu opener. Deliberately at the exact edge,
             * like Capture2Cloud's local menu marker.
             */
            ui_fill(
                R_MARKER.x,
                R_MARKER.y,
                10, 10,
                ((UiColour){
                    0xff, 0x00, 0xff, 0xff
                }));
        }

        ui_present();
    }

    release_remote_touch(
        &remote_touching);

    input_update(0);
    stream_disconnect();

    if (audio_alive)
        audio_exit();

    if (worker_alive)
        video_worker_stop();

    if (video_alive)
        video_exit();

    if (input_alive)
        input_exit();

    if (ui_alive)
        ui_shutdown();

    free(au);

    WHBLogPrintf(
        "bottom_screen Wii U: exit");

    WHBLogUdpDeinit();

    /* Keep ProcUI alive until application resources and logging are gone,
     * matching Capture2Cloud's hardware-validated HOME -> Quitter path. */
    proc_shutdown();

    return 0;
}
