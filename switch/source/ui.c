#include "ui.h"

#include <math.h>

static TTF_Font *g_small, *g_body;

void ui_bind(TTF_Font *small_font, TTF_Font *body_font)
{
    g_small = small_font;
    g_body = body_font;
}

static TTF_Font *pick(UiFont font)
{
    return (font == UI_FONT_SMALL) ? g_small : g_body;
}

void ui_text(SDL_Renderer *r, UiFont font, int x, int y, SDL_Color colour,
             const char *text)
{
    TTF_Font *f = pick(font);
    if (!f || !text || !text[0])
        return;
    SDL_Surface *surface = TTF_RenderUTF8_Blended(f, text, colour);
    if (!surface)
        return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surface);
    if (tex) {
        SDL_Rect dst = { x, y, surface->w, surface->h };
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(tex, colour.a);
        SDL_RenderCopy(r, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surface);
}

int ui_text_width(UiFont font, const char *text)
{
    TTF_Font *f = pick(font);
    int w = 0, h = 0;
    if (f && text && TTF_SizeUTF8(f, text, &w, &h) == 0)
        return w;
    return 0;
}

int ui_line_height(UiFont font)
{
    TTF_Font *f = pick(font);
    return f ? TTF_FontHeight(f) : 0;
}

void ui_fill(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color colour)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    SDL_Rect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

void ui_outline(SDL_Renderer *r, int x, int y, int w, int h, int thickness,
                SDL_Color colour)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    for (int i = 0; i < (thickness > 0 ? thickness : 1); i++) {
        SDL_Rect rect = { x + i, y + i, w - 2 * i, h - 2 * i };
        SDL_RenderDrawRect(r, &rect);
    }
}

/* Scanlines: SDL2 has no circle, and a square standing in for a round
 * control reads as a different one. */
void ui_fill_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    for (int dy = -radius; dy <= radius; dy++) {
        const int dx = (int)(sqrtf((float)(radius * radius - dy * dy)) + 0.5f);
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/* Eight-way symmetry, so the step is one point per octant rather than a
 * sine per degree. */
void ui_draw_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, colour.r, colour.g, colour.b, colour.a);
    int x = radius, y = 0, err = 1 - x;
    while (x >= y) {
        const int pts[8][2] = {
            { cx + x, cy + y }, { cx + y, cy + x }, { cx - y, cy + x },
            { cx - x, cy + y }, { cx - x, cy - y }, { cx - y, cy - x },
            { cx + y, cy - x }, { cx + x, cy - y },
        };
        for (int i = 0; i < 8; i++)
            SDL_RenderDrawPoint(r, pts[i][0], pts[i][1]);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}
