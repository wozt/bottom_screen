#ifndef BS_SWITCH_UI_H
#define BS_SWITCH_UI_H

/*
 * The drawing calls the on-screen pad needs, with the signatures it
 * already expects.
 *
 * The pad came from capture2cloud, whose homebrew had solved this
 * properly: round zones, a d-pad that gives diagonals, fingers bound
 * until they lift, a layout that can be moved and saved. These exist so
 * that vpad.c stays the file it is rather than being rewritten around
 * whatever this client happened to have.
 *
 * Three font sizes are declared because the pad asks for one by name;
 * this client carries two, and the third maps onto the nearest.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

typedef enum { UI_FONT_SMALL, UI_FONT_BODY, UI_FONT_TITLE } UiFont;

void ui_bind(TTF_Font *small_font, TTF_Font *body_font);

void ui_text(SDL_Renderer *r, UiFont font, int x, int y, SDL_Color colour,
             const char *text);
int  ui_text_width(UiFont font, const char *text);
int  ui_line_height(UiFont font);
void ui_fill(SDL_Renderer *r, int x, int y, int w, int h, SDL_Color colour);
void ui_outline(SDL_Renderer *r, int x, int y, int w, int h, int thickness,
                SDL_Color colour);
void ui_fill_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour);
void ui_draw_circle(SDL_Renderer *r, int cx, int cy, int radius, SDL_Color colour);

#endif
