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

static const Rect R_HOST       = { 330, 150, 620, 62 };
static const Rect R_PORT       = { 330, 232, 620, 62 };
static const Rect R_SCREEN     = { 330, 314, 620, 62 };
static const Rect R_CONNECT    = { 330, 410, 300, 68 };
static const Rect R_CLOSE      = { 650, 410, 300, 68 };
static const Rect R_MARKER     = { 1260, 0, 20, 20 };

static int hit(const Rect *r, int x, int y)
{
    return x >= r->x && y >= r->y &&
           x < r->x + r->w &&
           y < r->y + r->h;
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

static void draw_menu(const Settings *settings,
                      const char *note,
                      int connected,
                      int decoder_ok)
{
    char host[32];
    char port[16];

    settings_host_string(
        settings,
        host,
        sizeof(host));

    snprintf(
        port,
        sizeof(port),
        "%u",
        settings->port);

    ui_box(
        260, 76, 760, 500,
        ((UiColour){ 0x16, 0x1c, 0x26, 0xf0 }),
        UI_DIM);

    ui_text(
        330, 96,
        UI_SIZE_TITLE,
        UI_TEXT,
        "Bottom Screen");

    ui_text(
        330, 126,
        UI_SIZE_BODY,
        UI_DIM,
        "Native Wii U console client - first hardware bring-up");

    ui_box(
        R_HOST.x, R_HOST.y,
        R_HOST.w, R_HOST.h,
        UI_FIELD, UI_DIM);

    ui_text(
        R_HOST.x + 18,
        R_HOST.y + 6,
        UI_SIZE_BODY,
        UI_DIM,
        "Host");

    ui_text(
        R_HOST.x + 200,
        R_HOST.y + 6,
        UI_SIZE_BODY,
        UI_TEXT,
        "%s",
        host);

    ui_box(
        R_PORT.x, R_PORT.y,
        R_PORT.w, R_PORT.h,
        UI_FIELD, UI_DIM);

    ui_text(
        R_PORT.x + 18,
        R_PORT.y + 6,
        UI_SIZE_BODY,
        UI_DIM,
        "Port");

    ui_text(
        R_PORT.x + 200,
        R_PORT.y + 6,
        UI_SIZE_BODY,
        UI_TEXT,
        "%s",
        port);

    ui_box(
        R_SCREEN.x, R_SCREEN.y,
        R_SCREEN.w, R_SCREEN.h,
        UI_FIELD, UI_DIM);

    ui_text(
        R_SCREEN.x + 18,
        R_SCREEN.y + 6,
        UI_SIZE_BODY,
        UI_DIM,
        "Screen");

    ui_text(
        R_SCREEN.x + 200,
        R_SCREEN.y + 6,
        UI_SIZE_BODY,
        UI_TEXT,
        "%s",
        settings->screen == BS_SCREEN_TOP
            ? "Top"
            : "Bottom");

    draw_button(
        R_CONNECT,
        connected ? "DISCONNECT" : "CONNECT",
        connected ? UI_DANGER : UI_ACCENT);

    draw_button(
        R_CLOSE,
        connected ? "CLOSE MENU" : "HOME -> Quitter",
        UI_PANEL);

    ui_text(
        330, 502,
        UI_SIZE_BODY,
        decoder_ok ? UI_DIM : UI_DANGER,
        "%s",
        decoder_ok
            ? "Touch the top-right marker to reopen this menu."
            : "H264DEC is not ready.");

    if (note && note[0]) {
        ui_text(
            330, 540,
            UI_SIZE_BODY,
            UI_TEXT,
            "%s",
            note);
    }
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
    char current[32];
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
            1,
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
                "Invalid IPv4 address: %s",
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

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogUdpInit();
    WHBLogPrintf("bottom_screen Wii U: starting");

    proc_init();

    Settings settings;
    settings_load(&settings);

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
    int have_frame = 0;
    int remote_touching = 0;
    int last_touch_x = -1;
    int last_touch_y = -1;
    int need_keyframe = 0;

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

    while (proc_running()) {

        /*
         * HOME lifecycle: release H264DEC/GX2/AX before ProcUI gives the
         * foreground away, matching the path already validated in
         * Capture2Cloud.
         */
        if (proc_release_pending()) {
            WHBLogPrintf("suspend: disconnect");

            release_remote_touch(
                &remote_touching);

            stream_disconnect();

            if (audio_alive) {
                audio_exit();
                audio_alive = 0;
            }

            if (worker_alive) {
                video_worker_stop();
                worker_alive = 0;
            }

            if (video_alive) {
                video_exit();
                video_alive = 0;
            }

            if (input_alive) {
                input_exit();
                input_alive = 0;
            }

            if (ui_alive) {
                ui_shutdown();
                ui_alive = 0;
            }

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

        if (in.quit)
            break;

        /*
         * The reader owns the socket. If it died, join it here and make
         * the failure visible instead of keeping the last frame frozen.
         */
        if (!stream_connected() &&
            audio_alive) {

            release_remote_touch(
                &remote_touching);

            audio_exit();
            audio_alive = 0;

            snprintf(
                note,
                sizeof(note),
                "%s",
                stream_last_error()[0]
                    ? stream_last_error()
                    : "Disconnected");

            stream_disconnect();
            menu_open = 1;
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
                }
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
        }

        /*
         * Settings/menu touch.
         */
        if (in.tapped) {
            if (!menu_open &&
                hit(&R_MARKER,
                    in.touch_x,
                    in.touch_y)) {

                release_remote_touch(
                    &remote_touching);

                menu_open = 1;

            } else if (menu_open) {

                if (hit(&R_HOST,
                        in.touch_x,
                        in.touch_y)) {

                    if (stream_connected()) {
                        snprintf(
                            note,
                            sizeof(note),
                            "Disconnect before changing host");
                    } else {
                        edit_host(
                            &settings,
                            &input_alive,
                            note,
                            sizeof(note));

                        memset(
                            &in,
                            0,
                            sizeof(in));
                    }

                } else if (hit(&R_PORT,
                               in.touch_x,
                               in.touch_y)) {

                    if (stream_connected()) {
                        snprintf(
                            note,
                            sizeof(note),
                            "Disconnect before changing port");
                    } else {
                        edit_port(
                            &settings,
                            &input_alive,
                            note,
                            sizeof(note));

                        memset(
                            &in,
                            0,
                            sizeof(in));
                    }

                } else if (hit(&R_SCREEN,
                               in.touch_x,
                               in.touch_y)) {

                    settings.screen =
                        settings.screen == BS_SCREEN_BOTTOM
                            ? BS_SCREEN_TOP
                            : BS_SCREEN_BOTTOM;

                    if (stream_connected()) {
                        stream_send_screen(
                            settings.screen);

                        need_keyframe = 1;
                    }

                    settings_save(
                        &settings,
                        why,
                        sizeof(why));

                } else if (hit(&R_CONNECT,
                               in.touch_x,
                               in.touch_y)) {

                    if (stream_connected()) {
                        release_remote_touch(
                            &remote_touching);

                        input_update(0);
                        stream_disconnect();

                        if (audio_alive) {
                            audio_exit();
                            audio_alive = 0;
                        }

                        snprintf(
                            note,
                            sizeof(note),
                            "Disconnected");

                    } else if (!decoder_ok ||
                               !video_alive ||
                               !worker_alive) {

                        snprintf(
                            note,
                            sizeof(note),
                            "H264DEC is not ready");

                    } else {
                        char host[32];

                        settings_host_string(
                            &settings,
                            host,
                            sizeof(host));

                        why[0] = '\0';

                        /*
                         * A new TCP stream means new SPS/PPS and a new
                         * H.264 reference chain.
                         */
                        if (rebuild_decoder(
                                &video_alive,
                                &worker_alive,
                                why,
                                sizeof(why)) != 0) {

                            snprintf(
                                note,
                                sizeof(note),
                                "Decoder: %s",
                                why);

                        } else if (stream_connect(
                                       host,
                                       settings.port,
                                       why,
                                       sizeof(why)) != 0) {

                            snprintf(
                                note,
                                sizeof(note),
                                "Connect: %s",
                                why);

                        } else {
                            StreamInfo info;
                            stream_info(&info);

                            WHBLogPrintf(
                                "connected console=%d %dx%d @ %d",
                                info.console,
                                info.width,
                                info.height,
                                info.fps);

                            if (settings.screen !=
                                BS_SCREEN_BOTTOM) {
                                stream_send_screen(
                                    settings.screen);
                            }

                            if (info.audio_rate > 0) {
                                why[0] = '\0';

                                if (audio_init(
                                        48000,
                                        2,
                                        why,
                                        sizeof(why)) == 0) {

                                    audio_alive = 1;

                                } else {
                                    WHBLogPrintf(
                                        "audio disabled: %s",
                                        why);
                                }
                            }

                            settings_save(
                                &settings,
                                why,
                                sizeof(why));

                            note[0] = '\0';
                            menu_open = 0;
                            have_frame = 0;
                            need_keyframe = 0;
                        }
                    }

                } else if (hit(&R_CLOSE,
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
            !menu_open);

        /*
         * Real Wii U GamePad touchscreen -> emulated bottom screen.
         *
         * SDL reports the panel in the same 1280x720 coordinates as the
         * window. Convert from the letterboxed picture rectangle into
         * the server-announced stream coordinate space exactly once.
         */
        if (stream_connected() &&
            !menu_open &&
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

        if (menu_open ||
            !stream_connected()) {

            draw_menu(
                &settings,
                note,
                stream_connected(),
                decoder_ok);

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

    proc_shutdown();

    WHBLogPrintf(
        "bottom_screen Wii U: exit");

    WHBLogUdpDeinit();

    return 0;
}
