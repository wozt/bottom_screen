#include "ui.h"
#include "gx2_video.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <coreinit/memory.h>
#include <coreinit/time.h>
#include <whb/log.h>

/*
 * Benchmark temporaire:
 *   1 = H264DEC -> SDL_UpdateNVTexture -> SDL_RenderCopy
 *   0 = H264DEC -> gx2_video zero-copy
 *
 * Ne change ni le decodeur H264, ni l'audio AX, ni le reseau.
 */
#define UI_FORCE_SDL_VIDEO 0

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static TTF_Font     *g_body;
static TTF_Font     *g_title;
static int g_gamepad_only;
static int g_vsync = 1;

/*
 * Video texture.
 *
 * WiiU GX2's SDL renderer does not advertise NV12 directly. SDL2 sees
 * the FOURCC texture, creates its software YUV representation and a
 * native RGB backing texture supported by GX2, then converts during
 * SDL_UpdateNVTexture().
 */
static SDL_Texture *g_video_texture;

/*
 * Tiny transparent texture used to resynchronise SDL's software state
 * cache after our raw GX2 video draw.
 *
 * gx2_video_draw() changes GX2 shaders/textures behind SDL's back.
 * SDL therefore needs to observe a real TEXTURE -> COLOR transition
 * before drawing ordinary UI again.
 */
static SDL_Texture *g_state_reset_texture;

static int g_video_width;
static int g_video_height;
static int g_video_update_error_logged;

/*
 * Preferred path.
 *
 * SDL remains responsible for the window, text, touch and presentation,
 * but raw GX2 draws the NV12 picture between two SDL batches.
 */
static int g_gx2_video_ready;
static int g_gx2_video_failure_logged;

static uint64_t g_present_us_total;
static uint32_t g_present_us_max;
static unsigned g_present_count;

/*
 * Rendered strings, kept.
 *
 * TTF_RenderUTF8_Blended builds a surface and a texture every time it is
 * called. A status screen redraws a dozen strings sixty times a second,
 * and almost all of them are the same strings as last frame -- so they
 * are kept until the text changes. Small and fixed: a menu does not have
 * unbounded text, and an unbounded cache on a console with no swap is a
 * leak with a nicer name.
 */
#define CACHE_SIZE 48
typedef struct {
    char        text[96];
    int         size;
    UiColour    colour;
    SDL_Texture *texture;
    int         w, h;
    uint32_t    used;   /* for evicting the least recently wanted */
} TextEntry;

static TextEntry g_cache[CACHE_SIZE];
static uint32_t  g_clock;

void ui_set_output_mode(int gamepad_only)
{
    g_gamepad_only = gamepad_only ? 1 : 0;
}

void ui_set_vsync(int enabled)
{
    g_vsync = enabled ? 1 : 0;
}

void ui_restore_after_external_gx2(void)
{
    if (!g_renderer) {
        return;
    }

    SDL_RenderFlush(g_renderer);

    if (g_state_reset_texture) {
        SDL_Rect probe = { 0, 0, 1, 1 };

        SDL_RenderCopy(
            g_renderer,
            g_state_reset_texture,
            NULL,
            &probe);

        SDL_SetRenderDrawColor(
            g_renderer,
            0, 0, 0, 0);

        SDL_RenderFillRect(
            g_renderer,
            &probe);

        SDL_RenderFlush(g_renderer);
    }

    SDL_SetRenderDrawBlendMode(
        g_renderer,
        SDL_BLENDMODE_BLEND);
}

static SDL_Color to_sdl(UiColour c)
{
    SDL_Color s = { c.r, c.g, c.b, c.a };
    return s;
}

static TTF_Font *font_for(int size)
{
    return (size >= UI_SIZE_TITLE) ? g_title : g_body;
}

static TextEntry *entry_for(int size, UiColour colour, const char *text)
{
    TTF_Font *font = font_for(size);
    if (!font || !text || !text[0]) {
        return NULL;
    }

    TextEntry *oldest = &g_cache[0];
    for (int i = 0; i < CACHE_SIZE; i++) {
        TextEntry *e = &g_cache[i];
        if (e->texture && e->size == size && memcmp(&e->colour, &colour, sizeof(colour)) == 0 &&
            strncmp(e->text, text, sizeof(e->text) - 1) == 0) {
            e->used = ++g_clock;
            return e;
        }
        if (!e->texture) {
            oldest = e;
        } else if (oldest->texture && e->used < oldest->used) {
            oldest = e;
        }
    }

    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, to_sdl(colour));
    if (!surface) {
        return NULL;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    const int w = surface->w, h = surface->h;
    SDL_FreeSurface(surface);
    if (!texture) {
        return NULL;
    }

    if (oldest->texture) {
        /* A cached texture may still be referenced by SDL's queued draw
         * commands. Execute those commands before reclaiming it. */
        SDL_RenderFlush(g_renderer);
        SDL_DestroyTexture(oldest->texture);
    }
    snprintf(oldest->text, sizeof(oldest->text), "%s", text);
    oldest->size = size;
    oldest->colour = colour;
    oldest->texture = texture;
    oldest->w = w;
    oldest->h = h;
    oldest->used = ++g_clock;
    return oldest;
}

int ui_init(char *why, size_t why_size)
{
    if (SDL_Init(SDL_INIT_VIDEO |
                 SDL_INIT_EVENTS |
                 SDL_INIT_GAMECONTROLLER) != 0) {
        snprintf(why, why_size, "SDL_Init: %s", SDL_GetError());
        return -1;
    }
    if (TTF_Init() != 0) {
        snprintf(why, why_size, "TTF_Init: %s", TTF_GetError());
        return -1;
    }

    Uint32 window_flags = SDL_WINDOW_SHOWN;
    if (g_gamepad_only) {
        window_flags |= SDL_WINDOW_WIIU_GAMEPAD_ONLY;
    }

    g_window = SDL_CreateWindow("Bottom Screen", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                UI_WIDTH, UI_HEIGHT, window_flags);
    if (!g_window) {
        snprintf(why, why_size, "window: %s", SDL_GetError());
        return -1;
    }
    WHBLogPrintf("ui: output %s",
                 g_gamepad_only ? "GamePad only" : "TV + GamePad");
    const Uint32 renderer_flags =
        SDL_RENDERER_ACCELERATED |
        (g_vsync ? SDL_RENDERER_PRESENTVSYNC : 0);

    g_renderer = SDL_CreateRenderer(g_window, -1, renderer_flags);
    if (!g_renderer) {
        snprintf(why, why_size, "renderer: %s", SDL_GetError());
        return -1;
    }
    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    WHBLogPrintf("ui: vsync %s", g_vsync ? "on" : "off");

    g_state_reset_texture =
        SDL_CreateTexture(
            g_renderer,
            SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_STATIC,
            1,
            1);

    if (!g_state_reset_texture) {
        WHBLogPrintf(
            "ui: GX2 state reset texture unavailable: %s",
            SDL_GetError());
    } else {
        const uint32_t transparent = 0;

        SDL_UpdateTexture(
            g_state_reset_texture,
            NULL,
            &transparent,
            sizeof(transparent));

        SDL_SetTextureBlendMode(
            g_state_reset_texture,
            SDL_BLENDMODE_BLEND);

        WHBLogPrintf(
            "ui: GX2/SDL shader cache resync enabled");
    }

    /*
     * SDL has initialized GX2 by this point, which is exactly when the
     * raw NV12 renderer can safely allocate its shaders and buffers.
     */
#if UI_FORCE_SDL_VIDEO
    g_gx2_video_ready = 0;
#else
    g_gx2_video_ready =
        gx2_video_init() == 0;
#endif

    g_gx2_video_failure_logged = 0;

    g_present_us_total = 0;
    g_present_us_max = 0;
    g_present_count = 0;

    WHBLogPrintf(
        "ui video: %s",
#if UI_FORCE_SDL_VIDEO
        "FORCED SDL NV12 benchmark"
#else
        g_gx2_video_ready
            ? "direct GX2 NV12 enabled"
            : "GX2 unavailable; SDL YUV fallback"
#endif
    );

    /*
     * The console's own font, out of shared memory.
     *
     * Every Wii U has it, so nothing has to be shipped beside the .rpx
     * and nothing has to be found on the SD card at run time. It is the
     * same typeface the system menu uses, which is also why the result
     * looks like it belongs here.
     */
    void *font_data = NULL;
    uint32_t font_size = 0;
    if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font_data, &font_size) ||
        !font_data || font_size == 0) {
        snprintf(why, why_size, "the system font is not available");
        return -1;
    }
    WHBLogPrintf("ui: system font, %u bytes", (unsigned)font_size);

    /* Two RWops, because TTF_OpenFontRW takes ownership of the one it is
     * given and both fonts read the same block. */
    g_body = TTF_OpenFontRW(SDL_RWFromConstMem(font_data, (int)font_size), 1, UI_SIZE_BODY);
    g_title = TTF_OpenFontRW(SDL_RWFromConstMem(font_data, (int)font_size), 1, UI_SIZE_TITLE);
    if (!g_body || !g_title) {
        snprintf(why, why_size, "cannot open the system font: %s", TTF_GetError());
        return -1;
    }

    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(g_renderer, &info) == 0) {
        WHBLogPrintf("ui: renderer '%s', accelerated=%d", info.name ? info.name : "?",
                     (info.flags & SDL_RENDERER_ACCELERATED) ? 1 : 0);
    }
    return 0;
}

void ui_shutdown(void)
{
    /*
     * Raw GX2 resources must disappear while SDL/GX2 still exists.
     */
    gx2_video_shutdown();
    g_gx2_video_ready = 0;
    g_gx2_video_failure_logged = 0;

    if (g_video_texture) {
        SDL_DestroyTexture(g_video_texture);
        g_video_texture = NULL;
    }

    if (g_state_reset_texture) {
        SDL_DestroyTexture(g_state_reset_texture);
        g_state_reset_texture = NULL;
    }

    g_video_width = 0;
    g_video_height = 0;
    g_video_update_error_logged = 0;

    for (int i = 0; i < CACHE_SIZE; i++) {
        if (g_cache[i].texture) {
            SDL_DestroyTexture(g_cache[i].texture);
            g_cache[i].texture = NULL;
        }
    }
    if (g_body) TTF_CloseFont(g_body);
    if (g_title) TTF_CloseFont(g_title);
    g_body = g_title = NULL;
    TTF_Quit();
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    g_renderer = NULL;
    g_window = NULL;
    SDL_Quit();
}

void ui_poll(UiInput *in)
{
    const int was_touching = in->touching;
    in->quit = 0;
    in->tapped = 0;
    in->pressed = 0;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            in->quit = 1;
            break;
        /* The panel arrives as BOTH a finger and a mouse. The mouse is
         * taken because it is already in this window's pixels; the
         * finger would need the same multiplication done by hand. */
        case SDL_MOUSEBUTTONDOWN:
            in->touch_x = e.button.x;
            in->touch_y = e.button.y;
            in->touching = 1;
            break;
        case SDL_MOUSEMOTION:
            in->touch_x = e.motion.x;
            in->touch_y = e.motion.y;
            break;
        case SDL_MOUSEBUTTONUP:
            in->touch_x = e.button.x;
            in->touch_y = e.button.y;
            in->touching = 0;
            break;
        case SDL_FINGERDOWN:
            in->touching = 1;
            break;
        case SDL_FINGERUP:
            in->touching = 0;
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            in->pressed |= (1u << e.cbutton.button);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            /*
             * input.c owns the Wii U GamePad handle. Opening another
             * controller here would only add a second reference to the
             * same VPAD device.
             */
            break;
        default:
            break;
        }
    }

    /* Acted on the release, so a finger resting on a button is one
     * press and not sixty. */
    if (was_touching && !in->touching) {
        in->tapped = 1;
    }
}

void ui_begin(void)
{
    SDL_SetRenderDrawColor(g_renderer, UI_BG.r, UI_BG.g, UI_BG.b, 255);
    SDL_RenderClear(g_renderer);
}

static int ensure_video_texture(int width, int height)
{
    if (!g_renderer || width <= 0 || height <= 0) {
        return -1;
    }

    if (g_video_texture &&
        g_video_width == width &&
        g_video_height == height) {
        return 0;
    }

    if (g_video_texture) {
        SDL_DestroyTexture(g_video_texture);
        g_video_texture = NULL;
    }

    g_video_width = 0;
    g_video_height = 0;
    g_video_update_error_logged = 0;

    /*
     * NV12 isn't native to the Wii U SDL renderer, deliberately.
     *
     * SDL_CreateTexture() notices that and creates:
     *
     *   software NV12 texture
     *        +
     *   native renderer RGB texture
     *
     * SDL_UpdateNVTexture() then converts into the native texture.
     */
    g_video_texture =
        SDL_CreateTexture(g_renderer,
                          SDL_PIXELFORMAT_NV12,
                          SDL_TEXTUREACCESS_STREAMING,
                          width,
                          height);

    if (!g_video_texture) {
        WHBLogPrintf("ui video: cannot create NV12 %dx%d: %s",
                     width, height, SDL_GetError());
        return -1;
    }

    SDL_SetTextureScaleMode(g_video_texture, SDL_ScaleModeLinear);

    g_video_width = width;
    g_video_height = height;

    WHBLogPrintf(
        "ui video: NV12 texture ready %dx%d "
        "(SDL YUV fallback -> GX2 RGB)",
        width, height);

    return 0;
}

int ui_video_update_nv12(const uint8_t *luma,
                         const uint8_t *chroma,
                         int stride,
                         int width,
                         int height)
{
    if (!luma || !chroma ||
        width <= 0 || height <= 0 ||
        stride < width) {
        return -1;
    }

    g_video_width = width;
    g_video_height = height;

    /*
     * Preferred path:
     *
     * NV12 -> two GX2 textures -> shader -> RGB on GPU.
     */
    if (g_gx2_video_ready) {
        if (gx2_video_update(
                luma,
                chroma,
                stride,
                width,
                height) == 0) {
            return 0;
        }

        /*
         * Don't turn one GX2 problem into a black screen. The old path
         * remains available while we bring this up on real hardware.
         */
        if (!g_gx2_video_failure_logged) {
            WHBLogPrintf(
                "ui video: GX2 update failed; switching to SDL fallback");

            g_gx2_video_failure_logged = 1;
        }

        gx2_video_shutdown();
        g_gx2_video_ready = 0;
    }

    /*
     * Bring-up / emergency fallback only.
     */
    if (ensure_video_texture(width, height) != 0) {
        return -1;
    }

    if (SDL_UpdateNVTexture(
            g_video_texture,
            NULL,
            luma,
            stride,
            chroma,
            stride) != 0) {

        if (!g_video_update_error_logged) {
            WHBLogPrintf(
                "ui video: SDL_UpdateNVTexture failed: %s",
                SDL_GetError());

            g_video_update_error_logged = 1;
        }

        return -1;
    }

    return 0;
}

void ui_video_draw(void)
{
    if (g_video_width <= 0 ||
        g_video_height <= 0) {
        return;
    }

    if (g_gx2_video_ready) {
        /*
         * SDL batches its GX2 commands. Flush the background clear
         * BEFORE inserting our raw GX2 video draw.
         *
         * Any SDL menu/text queued afterwards is then naturally drawn
         * over the video.
         */
        SDL_RenderFlush(g_renderer);

        if (gx2_video_draw(
                UI_WIDTH,
                UI_HEIGHT) == 0) {

            /*
             * gx2_video_draw() bypasses SDL completely.
             *
             * SDL Wii U caches which shader/texture it THINKS GX2 has
             * bound. Merely restoring its context is not sufficient:
             * the software cache still says "COLOR shader active".
             *
             * Force two real SDL state transitions:
             *
             *   cached COLOR
             *       -> transparent texture => TEXTURE shader rebind
             *       -> transparent fill    => COLOR shader rebind
             *
             * Both draws are fully transparent and only one pixel.
             * The following real UI therefore starts with SDL's cache
             * and the actual GX2 state agreeing again.
             */
            ui_restore_after_external_gx2();

            return;
        }
    }

    /*
     * Software YUV conversion fallback.
     */
    if (!g_video_texture) {
        return;
    }

    SDL_Rect dst;

    if ((long long)g_video_width * UI_HEIGHT >
        (long long)g_video_height * UI_WIDTH) {

        dst.w = UI_WIDTH;
        dst.h =
            (int)((long long)UI_WIDTH *
                  g_video_height /
                  g_video_width);

        dst.x = 0;
        dst.y =
            (UI_HEIGHT - dst.h) / 2;

    } else {
        dst.h = UI_HEIGHT;
        dst.w =
            (int)((long long)UI_HEIGHT *
                  g_video_width /
                  g_video_height);

        dst.x =
            (UI_WIDTH - dst.w) / 2;

        dst.y = 0;
    }

    SDL_RenderCopy(
        g_renderer,
        g_video_texture,
        NULL,
        &dst);
}

void ui_present(void)
{
    const OSTime start =
        OSGetSystemTime();

    SDL_RenderPresent(g_renderer);

    const uint32_t us =
        (uint32_t)OSTicksToMicroseconds(
            OSGetSystemTime() - start);

    g_present_us_total += us;
    g_present_count++;

    if (us > g_present_us_max) {
        g_present_us_max = us;
    }
}

const char *ui_video_renderer_name(void)
{
#if UI_FORCE_SDL_VIDEO
    return "SDL NV12 BENCH";
#else
    return g_gx2_video_ready
        ? "GX2 zero-copy NV12"
        : "SDL NV12 fallback";
#endif
}

void ui_present_stats(uint32_t *avg_us,
                      uint32_t *max_us)
{
    if (avg_us) {
        *avg_us =
            g_present_count
                ? (uint32_t)(g_present_us_total /
                             g_present_count)
                : 0;
    }

    if (max_us) {
        *max_us = g_present_us_max;
    }
}

void ui_flush(void)
{
    SDL_RenderFlush(g_renderer);
}

void ui_fill(int x, int y, int w, int h, UiColour c)
{
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(g_renderer, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(g_renderer, &r);
}

void ui_box(int x, int y, int w, int h, UiColour fill, UiColour border)
{
    ui_fill(x, y, w, h, fill);
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(g_renderer, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(g_renderer, &r);
}

void ui_text(int x, int y, int size, UiColour c, const char *fmt, ...)
{
    char text[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    TextEntry *e = entry_for(size, c, text);
    if (!e) {
        return;
    }
    SDL_Rect dst = { x, y, e->w, e->h };
    SDL_RenderCopy(g_renderer, e->texture, NULL, &dst);
}

void ui_text_centred(int x, int y, int w, int h, int size, UiColour c, const char *text)
{
    TextEntry *e = entry_for(size, c, text);
    if (!e) {
        return;
    }
    SDL_Rect dst = { x + (w - e->w) / 2, y + (h - e->h) / 2, e->w, e->h };
    SDL_RenderCopy(g_renderer, e->texture, NULL, &dst);
}

int ui_text_width(int size, const char *text)
{
    int w = 0, h = 0;
    TTF_Font *font = font_for(size);
    if (font && text && TTF_SizeUTF8(font, text, &w, &h) == 0) {
        return w;
    }
    return 0;
}
