#ifndef CAPTURE2SWITCH_VPAD_H
#define CAPTURE2SWITCH_VPAD_H

#include <SDL2/SDL.h>

#include "padstate.h"

/*
 * An on-screen pad, for playing with the console flat on a table and
 * nothing plugged into it.
 *
 * The same idea as the browser page's touch overlay, and laid out the
 * same way: sticks and clusters positioned in units of one size so the
 * whole thing scales together, the d-pad a single round zone rather than
 * four buttons so diagonals work, and every control its own touch target
 * so several fingers do several things at once.
 *
 * It does not replace the physical pad -- both are read and combined, so
 * a Joy-Con in one hand and a thumb on the glass is a valid way to play.
 */

typedef enum {
    VPAD_LSTICK = 0,
    VPAD_RSTICK,
    VPAD_DPAD,
    VPAD_FACE,
    VPAD_L3,
    VPAD_R3,
    VPAD_LB,
    VPAD_LT,
    VPAD_RB,
    VPAD_RT,
    VPAD_SELECT,
    VPAD_START,
    VPAD_GUIDE,
    VPAD_COUNT
} VpadAnchor;

void vpad_init(void);

/* Which controls the machine being streamed actually has, one bit per
 * anchor. A DS has no ZL, no ZR and no stick; drawing one that goes
 * nowhere is worse than leaving it out. */
void vpad_set_present(unsigned mask);

/* Whether the overlay is shown and read at all. */
void vpad_set_enabled(int on);
int  vpad_enabled(void);

/* Edit mode: dragging a control moves it instead of pressing it. */
void vpad_set_editing(int on);
int  vpad_editing(void);

/* Back to where everything started. */
void vpad_reset_layout(void);

/* Changes with every move. Watched by whatever saves the settings, so a
 * button cannot be dragged without the saving noticing. */
unsigned vpad_layout_revision(void);

/* The overlay's colour and how strongly it is drawn.
 *
 * White on a bright scene is invisible, and this sits on top of whatever
 * the console happens to be showing -- so the colour has to be a choice.
 * `vpad_set_colour` takes an index into a short palette and wraps. */
void vpad_set_colour(int index);
int  vpad_colour(void);
int  vpad_colour_count(void);
const char *vpad_colour_name(void);
void vpad_set_opacity(int percent);
int  vpad_opacity(void);

/* Reads the touchscreen and updates every finger. A finger is bound to
 * whatever control it lands on and stays bound to it until it lifts, so
 * sliding off a button does not hand it to its neighbour.
 *
 * The screen is READ rather than waited on. Binding on a press event and
 * releasing on a release event left a button held forever whenever a
 * release did not arrive -- and it does not always arrive. A finger that
 * is gone is simply absent from the screen's own state, so a lost
 * release costs one pass instead of the rest of the session. */
void vpad_poll_touches(void);

/* Whether new touches may bind at all. Off while a local screen owns the
 * glass, so a tap on the menu is not also a button press underneath. */
void vpad_set_accepting(int on);

void vpad_touch_clear(void);

/* Combines what the glass is doing into `pad`, which already holds what
 * the physical controller is doing. Buttons are pressed if either says
 * so; a stick takes whichever deflection is larger. */
/* Whether a touch at this normalised point was probably meant for a
 * control. The caller uses it to keep whole-screen gestures -- the tap
 * at the top that opens the menu -- from firing on a missed press. */
int vpad_near_control(float nx, float ny);

void vpad_merge(PadState21 pad);

/* Draws the overlay, lit from `pad` -- which holds the physical
 * controller and the glass merged together by the time it gets here. So
 * pressing a Joy-Con button lights the one on screen, the same way the
 * browser page's overlay follows a real gamepad. */
void vpad_draw(SDL_Renderer *r, const PadState21 pad);

/* Layout as a single line for the config file, and back. The format is
 * "anchor:x,y" repeated, so an entry this build does not know is skipped
 * rather than shifting everything after it. */
void vpad_layout_to_string(char *out, size_t out_size);
void vpad_layout_from_string(const char *text);

#endif
