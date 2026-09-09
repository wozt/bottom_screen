#include "vpad.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "ui.h"

/* The screen this is laid out for. Positions are kept normalised, so a
 * different one would only change where things land, not the design. */
#define VPAD_W 1280
#define VPAD_H 720

/* One unit, everything else in multiples of it -- the same scheme the
 * browser overlay uses, so the two look and feel alike. */
#define U 44.0f

/* How much of the screen's height the stack of controls may use. The
 * rest is above a thumb, and a control nobody can reach is the same as
 * no control. */
#define VPAD_THUMB_REACH 0.62f

/* How far into a stick's circle counts as full deflection. Short of the
 * edge, because a thumb sliding to the rim of a circle it cannot see
 * would otherwise never quite reach 100. */
#define STICK_TRAVEL (1.55f * U)

/* How far off centre the d-pad zone starts registering a direction.
 * Independent per axis, which is what makes diagonals work: up and right
 * are two separate answers rather than one nearest button. */
#define DPAD_THRESHOLD (0.45f * U)

#define MAX_FINGERS 8

/* A bound finger that appears to have moved further than this in one
 * pass has not moved: its id has been handed to a different finger.
 *
 * The touchscreen reuses finger ids. Two fingers down, one lifts, and
 * the one still on the glass can come back reported under the id the
 * lifted one had -- so a binding keyed on the id alone survives, still
 * pressed, attached to a finger that is somewhere else entirely. That is
 * the button that never releases while the stick is being used, and it
 * is random because it depends on which id the hardware happens to
 * reuse.
 *
 * At 250 Hz a real finger moves a few pixels a pass. A hundred and fifty
 * is not a movement, it is a different finger. */
#define TELEPORT_PX 150.0f

typedef enum { SHAPE_CIRCLE, SHAPE_PILL, SHAPE_CLUSTER } VpadShape;

typedef struct {
    VpadShape shape;
    float radius;   /* circle and cluster */
    float w, h;     /* pill */
    const char *label;
} VpadStyle;

/* Sizes only. Where each one sits is in g_pos, which the player moves. */
/*
 * Everything scaled together.
 *
 * A Wii U's picture is 16:9 and fills the width, so the bands it gives
 * back are the narrowest of the three -- and the pad drawn at one size
 * for all of them spilled onto the screen there. One number, because
 * the controls are laid out in units of each other and scaling them
 * apart would be a different pad.
 */
static float g_scale = 1.0f;

/* Defined below, beside the table of sizes it walks. */
static float column_height_unscaled(void);

void vpad_set_scale(float scale)
{
    /*
     * Bounded by a thumb's reach as well as by the band's width.
     *
     * The scale came from the width of the black beside the picture and
     * from nothing else, so a wide band meant big buttons meant a tall
     * column -- and the topmost control, a shoulder, ended up in the
     * upper third of the screen. Nobody holding a console reaches
     * there. Whatever sits above that line may as well not be drawn, so
     * the buttons are made smaller instead.
     */
    const float tall = column_height_unscaled();
    if (tall > 0.0f) {
        const float fits = (VPAD_H * VPAD_THUMB_REACH) / tall;
        if (scale > fits) scale = fits;
    }
    if (scale < 0.4f) scale = 0.4f;
    if (scale > 1.5f) scale = 1.5f;
    g_scale = scale;
}

static const VpadStyle STYLE_BASE[VPAD_COUNT] = {
    [VPAD_LSTICK] = {SHAPE_CIRCLE,  1.8f * U, 0, 0, NULL},
    [VPAD_RSTICK] = {SHAPE_CIRCLE,  1.8f * U, 0, 0, NULL},
    [VPAD_DPAD]   = {SHAPE_CIRCLE, 1.65f * U, 0, 0, NULL},
    [VPAD_FACE]   = {SHAPE_CLUSTER, 2.0f * U, 0, 0, NULL},
    [VPAD_L3]     = {SHAPE_CIRCLE, 0.575f * U, 0, 0, "L3"},
    [VPAD_R3]     = {SHAPE_CIRCLE, 0.575f * U, 0, 0, "R3"},
    [VPAD_LB]     = {SHAPE_PILL,   0, 1.8f * U, 1.1f * U, "L"},
    [VPAD_LT]     = {SHAPE_PILL,   0, 1.8f * U, 1.1f * U, "ZL"},
    [VPAD_RB]     = {SHAPE_PILL,   0, 1.8f * U, 1.1f * U, "R"},
    [VPAD_RT]     = {SHAPE_PILL,   0, 1.8f * U, 1.1f * U, "ZR"},
    [VPAD_SELECT] = {SHAPE_PILL,   0, 1.8f * U, 0.9f * U, "-"},
    [VPAD_START]  = {SHAPE_PILL,   0, 1.8f * U, 0.9f * U, "+"},
    [VPAD_GUIDE]  = {SHAPE_CIRCLE, 0.50f * U, 0, 0, "H"},
};

/*
 * How tall the busier column is at scale 1, in pixels.
 *
 * The same walk stack_column does, without placing anything: the bottom
 * inset, then every control's own height with a gap between, and the
 * face cluster's extra claimed on both sides. Counting it here rather
 * than writing a number down means the two cannot drift apart.
 */
static float column_height_unscaled(void)
{
    const int cols[2][5] = {
        { VPAD_SELECT, VPAD_DPAD, VPAD_LSTICK, VPAD_LB, VPAD_LT },
        { VPAD_START,  VPAD_FACE, VPAD_RSTICK, VPAD_RB, VPAD_RT },
    };
    float tallest = 0.0f;
    for (int c = 0; c < 2; c++) {
        float total = 0.6f * U;
        float pending = 0.0f;
        for (int i = 0; i < 5; i++) {
            const VpadStyle st = STYLE_BASE[cols[c][i]];
            const float half = (st.shape == SHAPE_PILL) ? st.h / 2 : st.radius;
            const float extra = (st.shape == SHAPE_CLUSTER) ? 0.45f * U : 0.0f;
            total += half + pending + extra + half + 0.35f * U;
            pending = extra;
        }
        if (total > tallest)
            tallest = total;
    }
    return tallest;
}

/* The sizes actually used, which are the ones above scaled. */
static VpadStyle style_of(int a)
{
    VpadStyle st = STYLE_BASE[a];
    st.radius *= g_scale;
    st.w *= g_scale;
    st.h *= g_scale;
    return st;
}

/* Where everything starts, taken from the browser overlay's own layout
 * so the two are the same pad.
 *
 * Asymmetric, which is the arrangement a Switch player expects: on the
 * left the stick sits above the d-pad, on the right the face buttons sit
 * above the stick.
 *
 * Everything else lives LOW, in a half-unit column at each outer edge --
 * triggers at the very bottom, shoulders above them, minus and plus
 * above those. They were up in the top corners here, which is nowhere
 * near a thumb that is already holding the console: the whole point of
 * that column is that the four of them plus select or start are within
 * one thumb's reach without letting go, and clear of the stick and
 * d-pad cluster.
 *
 * Home stays in the middle of the bottom edge. It opens the remote
 * console's system menu, so a deliberate small reach guards against
 * catching it by accident -- and it can be dragged closer by anyone who
 * would rather have it to hand. */
static const SDL_FPoint DEFAULT_POS[VPAD_COUNT] = {
    [VPAD_LSTICK] = {4.800f * U, VPAD_H - 6.000f * U},
    [VPAD_DPAD]   = {4.800f * U, VPAD_H - 2.050f * U},
    [VPAD_RSTICK] = {VPAD_W - 4.800f * U, VPAD_H - 2.200f * U},
    [VPAD_FACE]   = {VPAD_W - 4.800f * U, VPAD_H - 6.200f * U},
    [VPAD_L3]     = {7.375f * U, VPAD_H - 5.275f * U},
    [VPAD_R3]     = {VPAD_W - 7.375f * U, VPAD_H - 1.475f * U},
    [VPAD_LT]     = {1.400f * U, VPAD_H - 0.950f * U},
    [VPAD_LB]     = {1.400f * U, VPAD_H - 2.250f * U},
    [VPAD_RT]     = {VPAD_W - 1.400f * U, VPAD_H - 0.950f * U},
    [VPAD_RB]     = {VPAD_W - 1.400f * U, VPAD_H - 2.250f * U},
    [VPAD_SELECT] = {1.400f * U, VPAD_H - 3.450f * U},
    [VPAD_START]  = {VPAD_W - 1.400f * U, VPAD_H - 3.450f * U},
    [VPAD_GUIDE]  = {VPAD_W * 0.5f, VPAD_H - 0.900f * U},
};

/* Normalised, so the layout survives a change of resolution and reads
 * sensibly in the config file. */
static SDL_FPoint g_pos[VPAD_COUNT];

static int g_enabled = 0;
static int g_editing = 0;
/* Off while a local screen owns the glass, so a tap on the menu is not
 * also a button press underneath it. */
static int g_accepting = 1;

/* Bumped on every change to where things are. Whoever saves the
 * settings watches this rather than being told, so a position cannot be
 * moved without the saving noticing. */
static unsigned g_revision = 0;

typedef struct {
    int active;
    SDL_FingerID id;
    int anchor;
    int sub;        /* which face button, when the anchor is the cluster */
    float x, y;     /* pixels */
} VpadFinger;

static VpadFinger g_fingers[MAX_FINGERS];

/* Touch events arrive on the frame loop; the pad is sampled on the
 * controller thread. One lock, held only for the handful of
 * instructions that touch the array. */
static SDL_mutex *g_lock = NULL;
#define VPAD_LOCK()   do { if (g_lock) SDL_LockMutex(g_lock); } while (0)
#define VPAD_UNLOCK() do { if (g_lock) SDL_UnlockMutex(g_lock); } while (0)

/* The four face buttons, by position rather than by name: the bottom one
 * is the bottom one wherever it ends up, which is how the rest of this
 * program talks about them. */
static const struct { float dx, dy; int slot; const char *label; } FACE[4] = {
    {0.0f, -1.30f * U, PAD_Y, "X"},  /* top */
    {-1.30f * U, 0.0f, PAD_X, "Y"},  /* left */
    {1.30f * U, 0.0f, PAD_B, "A"},   /* right */
    {0.0f, 1.30f * U, PAD_A, "B"},   /* bottom */
};
#define FACE_RADIUS (0.72f * U)

/* The overlay's colour, chosen rather than fixed.
 *
 * It was white throughout, which is invisible on a bright scene -- and
 * this is drawn on top of whatever the console happens to be showing, so
 * there is no colour that works against all of it. A short palette and
 * an opacity are enough: pick something the game is not. */
static const struct { const char *name; Uint8 r, g, b; } PALETTE[] = {
    {"white",   255, 255, 255},
    {"black",     0,   0,   0},
    {"red",     255,  70,  70},
    {"orange",  255, 150,  40},
    {"yellow",  255, 225,  60},
    {"green",    90, 220, 120},
    {"cyan",     80, 220, 235},
    {"blue",     90, 150, 255},
    {"magenta", 240, 100, 230},
};
#define PALETTE_COUNT ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

/*
 * Which controls this console has.
 *
 * The pad was written for a Switch, which has every one of them; the
 * machines being streamed to do not. A DS has no ZL, no ZR and no stick
 * at all, and drawing a control that goes nowhere is worse than leaving
 * it out -- somebody presses it and concludes the client is broken.
 * One bit per anchor, all of them until told otherwise.
 */
static unsigned g_present = 0xFFFFFFFFu;

/*
 * The picture, which no control may sit on.
 *
 * A button over the streamed screen is a button over the thing being
 * played on -- and on a DS or a 3DS that screen is the stylus surface,
 * so a control there would take touches meant for the game. Dragging
 * one stops at the edge rather than being allowed across and quietly
 * clamped somewhere else: a wall you can feel, not a correction you
 * discover afterwards.
 */
static SDL_Rect g_forbidden = {0, 0, 0, 0};
static int g_custom;   /* somebody's own arrangement */

void vpad_reset_layout(void);

/*
 * The picture moved, so the bands did.
 *
 * The default arrangement is computed from them, so it is recomputed
 * here -- but only while it is still the default. Somebody's own
 * arrangement stays where they put it, and the wall in anchor_centre
 * keeps it off the picture whatever shape that turns out to be.
 */
void vpad_set_forbidden(SDL_Rect r)
{
    const int moved = (r.x != g_forbidden.x || r.w != g_forbidden.w);
    g_forbidden = r;
    if (moved && !g_custom)
        vpad_reset_layout();
}

/* Pushes a centre out of the picture, to whichever side it is nearer.
 * Vertical is left alone: the bands run the full height, so sliding up
 * and down inside one is free. */
static float keep_out(float cx, float half)
{
    if (g_forbidden.w <= 0 || g_forbidden.h <= 0)
        return cx;

    const float left  = (float)g_forbidden.x;
    const float right = (float)(g_forbidden.x + g_forbidden.w);
    if (cx + half <= left || cx - half >= right)
        return cx;                       /* already clear */

    const float mid = (left + right) * 0.5f;
    return (cx < mid) ? left - half : right + half;
}

void vpad_set_present(unsigned mask) { g_present = mask; }
static int shown(int anchor) { return (g_present >> anchor) & 1u; }

static int g_colour = 0;
static int g_opacity = 100;

/* One tint at a given strength. Opacity scales every layer together, so
 * turning it down fades the whole pad rather than only its fill. */
static SDL_Color tint(int alpha) {
    SDL_Color c = {PALETTE[g_colour].r, PALETTE[g_colour].g, PALETTE[g_colour].b,
                   (Uint8)(alpha * g_opacity / 100)};
    return c;
}

/* The label is the pad's own colour, not a contrasting one.
 *
 * Drawn at full strength against a fill drawn at a fifth of it, which is
 * what keeps it readable: the difference is the alpha, not the hue. A
 * pad set to green should be green throughout rather than green rings
 * around white letters. */
static SDL_Color label_colour(void) {
    return tint(235);
}

#define COL_BODY  tint(46)
#define COL_EDGE  tint(150)
#define COL_KNOB  tint(90)
#define COL_ON    tint(215)
#define COL_LABEL label_colour()
static const SDL_Color COL_EDIT   = {255, 220,   0, 200};

void vpad_set_colour(int index) {
    if (index < 0) index = PALETTE_COUNT - 1;
    g_colour = index % PALETTE_COUNT;
}
int vpad_colour(void) { return g_colour; }
const char *vpad_colour_name(void) { return PALETTE[g_colour].name; }
int vpad_colour_count(void) { return PALETTE_COUNT; }

void vpad_set_opacity(int percent) {
    if (percent < 20) percent = 20;
    if (percent > 100) percent = 100;
    g_opacity = percent;
}
int vpad_opacity(void) { return g_opacity; }

/*
 * Where each side's stick sits relative to its thumb control.
 *
 * A Switch is asymmetric -- stick above the d-pad on the left, face
 * buttons above the stick on the right -- and a PlayStation is not. Both
 * are things people are used to, and which one a hand expects is not
 * something this can know, so it is a choice. One per side, because the
 * two hands are not obliged to agree.
 *
 * It rearranges the default layout and nothing else. Once somebody has
 * dragged their own arrangement, moving their controls out from under
 * them because a menu changed would be the wrong kind of helpful.
 */
static int g_stick_below[2] = {0, 1};   /* left above, right below */

void vpad_set_stick_below(int side, int below)
{
    if (side < 0 || side > 1)
        return;
    g_stick_below[side] = below ? 1 : 0;
    if (!g_custom)
        vpad_reset_layout();
}

int vpad_stick_below(int side)
{
    return (side < 0 || side > 1) ? 0 : g_stick_below[side];
}

/*
 * The default arrangement: two columns, stacked from the bottom.
 *
 * capture2cloud's own defaults are spread across a whole screen, because
 * its overlay sits on top of one. Here the pad lives in the black band
 * either side of the picture, which is a fifth of that width, and
 * pushing those positions into it piled every control on the same spot.
 * So the defaults are computed for the bands instead of borrowed.
 *
 * Bottom-up, because that is the end a thumb reaches: menu button,
 * thumb control and stick in whichever order the side was asked for,
 * then the shoulder and the trigger above them. HOME is the exception,
 * pinned to the top of its column and out of the way -- it is the one
 * that stops the game, and it should take a deliberate reach.
 */
static void stack_column(float cx, const int *order, int count, float top_pad)
{
    /* Bottom edge upwards, one control at a time. */
    float y = VPAD_H - 0.6f * U * g_scale;
    float pending = 0.0f;                     /* owed to the last control */

    for (int i = 0; i < count; i++) {
        const int a = order[i];
        if (a < 0)
            continue;
        const VpadStyle st = style_of(a);
        const float half = (st.shape == SHAPE_PILL) ? st.h / 2 : st.radius;

        /*
         * The face cluster asks for more room than the others.
         *
         * Its four buttons sit 1.30 units out with a radius of 0.72, so
         * it reaches 2.02 units while declaring 2.0 -- and stacked on
         * the same small gap as everything else it grazed the trigger
         * above it and the stick below. The extra is claimed on both
         * sides, which is why it is carried over to the next one rather
         * than only subtracted here.
         */
        const float gap = 0.35f * U * g_scale;
        const float extra = (st.shape == SHAPE_CLUSTER) ? 0.45f * U * g_scale : 0.0f;

        y -= half + pending + extra;
        g_pos[a].x = cx / VPAD_W;
        g_pos[a].y = y / VPAD_H;
        y -= half + gap;
        pending = extra;
    }
    (void)top_pad;
}

void vpad_reset_layout(void) {
    for (int i = 0; i < VPAD_COUNT; i++) {
        g_pos[i].x = DEFAULT_POS[i].x / VPAD_W;
        g_pos[i].y = DEFAULT_POS[i].y / VPAD_H;
    }

    /* Where the two columns sit: the middle of each band, or a guess at
     * one when nobody has said where the picture is yet. */
    const float lband = (g_forbidden.w > 0) ? (float)g_forbidden.x : VPAD_W * 0.12f;
    const float rstart = (g_forbidden.w > 0)
                       ? (float)(g_forbidden.x + g_forbidden.w) : VPAD_W * 0.88f;
    const float lcx = lband * 0.5f;
    const float rcx = rstart + (VPAD_W - rstart) * 0.5f;

    /* Left: SELECT at the bottom, then the d-pad and its stick in the
     * order this side was asked for, then L and ZL above. */
    int left[5];
    int n = 0;
    left[n++] = VPAD_SELECT;
    if (g_stick_below[0]) { left[n++] = VPAD_LSTICK; left[n++] = VPAD_DPAD; }
    else                  { left[n++] = VPAD_DPAD;   left[n++] = VPAD_LSTICK; }
    left[n++] = VPAD_LB;
    left[n++] = VPAD_LT;
    stack_column(lcx, left, n, 0);

    int right[5];
    n = 0;
    right[n++] = VPAD_START;
    if (g_stick_below[1]) { right[n++] = VPAD_RSTICK; right[n++] = VPAD_FACE; }
    else                  { right[n++] = VPAD_FACE;   right[n++] = VPAD_RSTICK; }
    right[n++] = VPAD_RB;
    right[n++] = VPAD_RT;
    stack_column(rcx, right, n, 0);

    /* The one that resists gravity. */
    {
        const VpadStyle st = style_of(VPAD_GUIDE);
        g_pos[VPAD_GUIDE].x = rcx / VPAD_W;
        g_pos[VPAD_GUIDE].y = (0.9f * U * g_scale + st.radius) / VPAD_H;
    }

    /* Stick clicks are not offered by any console here; park them out of
     * the way rather than leave them where the borrowed defaults put
     * them. */
    g_pos[VPAD_L3].x = g_pos[VPAD_R3].x = -1.0f;

    g_custom = 0;
    g_revision++;
}

unsigned vpad_layout_revision(void) {
    return g_revision;
}

void vpad_init(void) {
    vpad_reset_layout();
    memset(g_fingers, 0, sizeof(g_fingers));
    if (!g_lock) {
        g_lock = SDL_CreateMutex();
    }
    hidInitializeTouchScreen();
}

/* Caller holds the lock. */
static void move_finger(VpadFinger *f, float px, float py) {
    f->x = px;
    f->y = py;

    if (g_editing) {
        /* The control follows the finger, kept far enough inside the
         * screen that it can always be grabbed again. */
        const VpadStyle sv = style_of(f->anchor); const VpadStyle *st = &sv;
        const float half = (st->shape == SHAPE_PILL) ? st->w / 2 : st->radius;
        float cx = f->x, cy = f->y;
        if (cx < half) cx = half;
        if (cx > VPAD_W - half) cx = VPAD_W - half;
        if (cy < half) cy = half;
        if (cy > VPAD_H - half) cy = VPAD_H - half;
        cx = keep_out(cx, half);
        if (cx < half) cx = half;
        if (cx > VPAD_W - half) cx = VPAD_W - half;
        if (g_pos[f->anchor].x != cx / VPAD_W || g_pos[f->anchor].y != cy / VPAD_H) {
            g_pos[f->anchor].x = cx / VPAD_W;
            g_pos[f->anchor].y = cy / VPAD_H;
            g_custom = 1;
            g_revision++;
        }
    }
}

static int hit_test(float px, float py, int *sub_out);
static VpadFinger *find_finger(SDL_FingerID id);

/* Controls a finger is meant to travel across: the sticks and the d-pad
 * are aimed by dragging, and in edit mode everything is. Everything else
 * is a button, and a thumb sliding off a button releases it. */
static int is_draggable(int anchor) {
    return g_editing || anchor == VPAD_LSTICK || anchor == VPAD_RSTICK || anchor == VPAD_DPAD;
}

/* Whether this finger is still within what it is bound to. */
static int still_on(const VpadFinger *f, float px, float py) {
    int sub = -1;
    return hit_test(px, py, &sub) == f->anchor && (f->sub < 0 || sub == f->sub);
}

void vpad_set_accepting(int on) {
    if (!on && g_accepting) {
        vpad_touch_clear();
    }
    g_accepting = on ? 1 : 0;
}

/* Reads the touchscreen itself instead of waiting to be told.
 *
 * A button could stay pressed forever, and the reason was structural: a
 * finger was bound when a press event arrived and released when a
 * release event arrived, so a release that never arrived left it held
 * for good. That happens -- a lifted finger can vanish from the hardware
 * without the event layer noticing, and there is no timeout that makes
 * "still pressed" true again afterwards.
 *
 * The screen's own state has no such gap: a finger that is gone is
 * simply not in the list. Anything bound to an id that is no longer
 * there is released, every pass, so a lost release costs four
 * milliseconds instead of the rest of the session.
 *
 * Called from the controller thread, at its rate rather than the
 * drawing's. */
void vpad_poll_touches(void) {
    if (!g_enabled) {
        return;
    }

    HidTouchScreenState screen;
    memset(&screen, 0, sizeof(screen));
    const int have = (hidGetTouchScreenStates(&screen, 1) > 0);
    const int count = have ? screen.count : 0;

    VPAD_LOCK();

    /* Anything no longer on the glass lets go. */
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (!g_fingers[i].active) {
            continue;
        }
        int still_there = 0;
        for (int t = 0; t < count; t++) {
            if ((SDL_FingerID)screen.touches[t].finger_id == g_fingers[i].id) {
                still_there = 1;
                break;
            }
        }
        if (!still_there) {
            g_fingers[i].active = 0;
        }
    }

    if (!g_accepting) {
        VPAD_UNLOCK();
        return;
    }

    for (int t = 0; t < count; t++) {
        const SDL_FingerID id = (SDL_FingerID)screen.touches[t].finger_id;
        const float px = (float)screen.touches[t].x;
        const float py = (float)screen.touches[t].y;

        VpadFinger *f = find_finger(id);
        if (f) {
            const float dx = px - f->x, dy = py - f->y;
            if (dx * dx + dy * dy > TELEPORT_PX * TELEPORT_PX) {
                /* The id was handed to somebody else. */
                f->active = 0;
                f = NULL;
            } else if (!is_draggable(f->anchor) && !still_on(f, px, py)) {
                /* A thumb that has slid off a button releases it, and is
                 * free to land on whatever it slid onto -- the binding
                 * below picks it up on this same pass. */
                f->active = 0;
                f = NULL;
            }
        }
        if (f) {
            move_finger(f, px, py);
            continue;
        }

        /* New, or newly freed: bound to whatever it is on, and to
         * nothing if it is on nothing. */
        int sub = -1;
        const int a = hit_test(px, py, &sub);
        if (a < 0) {
            continue;
        }
        for (int i = 0; i < MAX_FINGERS; i++) {
            if (!g_fingers[i].active) {
                g_fingers[i].active = 1;
                g_fingers[i].id = id;
                g_fingers[i].anchor = a;
                g_fingers[i].sub = sub;
                g_fingers[i].x = px;
                g_fingers[i].y = py;
                break;
            }
        }
    }

    VPAD_UNLOCK();
}

void vpad_set_enabled(int on) {
    g_enabled = on ? 1 : 0;
    if (!g_enabled) {
        vpad_touch_clear();
        g_editing = 0;
    }
}
int vpad_enabled(void) { return g_enabled; }

void vpad_set_editing(int on) {
    g_editing = on ? 1 : 0;
    vpad_touch_clear();
}
int vpad_editing(void) { return g_editing; }

/*
 * Where a control actually is, which is not quite where it is stored.
 *
 * The picture is pushed out of here rather than only when something is
 * dragged, because a stored position predates the picture it now has to
 * avoid: a layout saved against a 4:3 stream, or the defaults, would
 * otherwise sit on top of a 16:9 one. Enforcing it on every read means
 * there is no path by which a control ends up on the screen.
 */
static void anchor_centre(int a, float *x, float *y) {
    const VpadStyle sv = style_of(a); const VpadStyle *st = &sv;
    const float half = (st->shape == SHAPE_PILL) ? st->w / 2 : st->radius;
    float cx = g_pos[a].x * VPAD_W;
    cx = keep_out(cx, half);
    if (cx < half) cx = half;
    if (cx > VPAD_W - half) cx = VPAD_W - half;
    *x = cx;
    *y = g_pos[a].y * VPAD_H;
}

/* Which control is under this point, or -1. Searched from the last drawn
 * backwards, so the one on top of a pile is the one that gets it. */
static int hit_test(float px, float py, int *sub_out) {
    for (int a = VPAD_COUNT - 1; a >= 0; a--) {
        if (!shown(a)) continue;
        float cx, cy;
        anchor_centre(a, &cx, &cy);
        const VpadStyle sv = style_of(a); const VpadStyle *st = &sv;

        if (st->shape == SHAPE_PILL) {
            if (fabsf(px - cx) <= st->w / 2 && fabsf(py - cy) <= st->h / 2) {
                if (sub_out) *sub_out = -1;
                return a;
            }
            continue;
        }

        const float dx = px - cx, dy = py - cy;
        if (st->shape == SHAPE_CLUSTER) {
            /* Each face button on its own, except while moving the
             * cluster, where the whole thing is one target. */
            if (g_editing) {
                if (dx * dx + dy * dy <= st->radius * st->radius) {
                    if (sub_out) *sub_out = -1;
                    return a;
                }
                continue;
            }
            for (int i = 0; i < 4; i++) {
                const float bx = dx - FACE[i].dx, by = dy - FACE[i].dy;
                if (bx * bx + by * by <= FACE_RADIUS * FACE_RADIUS) {
                    if (sub_out) *sub_out = i;
                    return a;
                }
            }
            continue;
        }

        if (dx * dx + dy * dy <= st->radius * st->radius) {
            if (sub_out) *sub_out = -1;
            return a;
        }
    }
    return -1;
}

static VpadFinger *find_finger(SDL_FingerID id) {
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (g_fingers[i].active && g_fingers[i].id == id) {
            return &g_fingers[i];
        }
    }
    return NULL;
}

void vpad_touch_clear(void) {
    VPAD_LOCK();
    memset(g_fingers, 0, sizeof(g_fingers));
    VPAD_UNLOCK();
}

static void press(PadState21 pad, int slot, int value) {
    if (value > pad[slot]) {
        pad[slot] = (int8_t)value;
    }
}

static void deflect(PadState21 pad, int slot, int value) {
    if (abs(value) > abs(pad[slot])) {
        pad[slot] = (int8_t)value;
    }
}

/* Whether a point on the glass is close enough to a control that a
 * touch there was probably meant for it.
 *
 * Used to keep the tap-at-the-top gesture that opens the menu away from
 * the pad. The shoulders, the triggers, minus, plus and HOME all live
 * along the top edge, so a thumb aiming for one of those and missing was
 * landing in the menu zone -- and opening a menu is a poor thing to get
 * for a missed press in the middle of a game.
 *
 * A margin around each control rather than a fixed strip, because the
 * controls can be dragged: a strip chosen for the default layout would
 * stop matching them the moment anyone moved anything.
 */
int vpad_near_control(float nx, float ny) {
    if (!vpad_enabled()) {
        return 0;
    }
    const float x = nx * VPAD_W;
    const float y = ny * VPAD_H;
    /* Three times each control's own size. Generous on purpose: the
     * cost of guarding too much is a gesture that needs aiming at empty
     * screen, and the cost of guarding too little is the gesture firing
     * on a missed press, which is the thing being fixed. */
    const float margin = 3.0f;
    for (int i = 0; i < VPAD_COUNT; i++) {
        const VpadStyle sv = style_of(i); const VpadStyle *style = &sv;
        const SDL_FPoint p = g_pos[i];
        switch (style->shape) {
            case SHAPE_PILL: {
                const float w = style->w * 0.5f * margin;
                const float h = style->h * 0.5f * margin;
                if (fabsf(x - p.x) <= w && fabsf(y - p.y) <= h) return 1;
                break;
            }
            case SHAPE_CLUSTER: {
                /* The four face buttons sit a little over one unit out
                 * from the centre, so the cluster's reach is its own
                 * radius plus that. */
                const float r = (style->radius + 1.30f * U) * margin;
                const float dx = x - p.x, dy = y - p.y;
                if (dx * dx + dy * dy <= r * r) return 1;
                break;
            }
            default: {
                const float r = style->radius * margin;
                const float dx = x - p.x, dy = y - p.y;
                if (dx * dx + dy * dy <= r * r) return 1;
                break;
            }
        }
    }
    return 0;
}

void vpad_merge(PadState21 pad) {
    if (!g_enabled || g_editing) {
        return;
    }
    VPAD_LOCK();
    for (int i = 0; i < MAX_FINGERS; i++) {
        const VpadFinger *f = &g_fingers[i];
        if (!f->active) {
            continue;
        }
        float cx, cy;
        anchor_centre(f->anchor, &cx, &cy);
        const float dx = f->x - cx, dy = f->y - cy;

        switch (f->anchor) {
            case VPAD_LSTICK:
            case VPAD_RSTICK: {
                float nx = dx / STICK_TRAVEL, ny = dy / STICK_TRAVEL;
                const float len = sqrtf(nx * nx + ny * ny);
                if (len > 1.0f) { nx /= len; ny /= len; }
                const int x_slot = (f->anchor == VPAD_LSTICK) ? PAD_LX : PAD_RX;
                const int y_slot = (f->anchor == VPAD_LSTICK) ? PAD_LY : PAD_RY;
                deflect(pad, x_slot, (int)(nx * 100.0f));
                /*
                 * Negated: a screen's y grows downward and a stick's
                 * does not. Reading the finger's offset straight into
                 * the axis made the on-screen sticks answer up with down
                 * -- and only the on-screen ones, because the console's
                 * own sticks already report the other way round and both
                 * end up in the same merged state. Two halves of one
                 * control disagreeing is worse than either convention.
                 */
                deflect(pad, y_slot, (int)(-ny * 100.0f));
                break;
            }
            case VPAD_DPAD: {
                /*
                 * Eight sectors of the circle, not two thresholds on a
                 * square.
                 *
                 * Crossing an x threshold and a y threshold
                 * independently does give diagonals, but only in the
                 * corners of the square they describe -- where the
                 * thumb has already left the cross -- while each
                 * straight direction takes a third of the width.
                 * Sectors give all eight the same forty-five degrees,
                 * and the diagonals land where the circle's diagonals
                 * are, which is the shape this is drawn as.
                 */
                const float len = sqrtf(dx * dx + dy * dy);
                if (len <= DPAD_THRESHOLD)
                    break;          /* a dead middle, so resting is not a direction */

                float a = atan2f(dy, dx) * 180.0f / 3.14159265f;
                if (a < 0.0f) a += 360.0f;
                /* Shifted by half a sector so each direction is centred
                 * on its own axis rather than starting at it. */
                switch ((int)((a + 22.5f) / 45.0f) % 8) {
                case 0: press(pad, PAD_RIGHT, 100); break;
                case 1: press(pad, PAD_RIGHT, 100); press(pad, PAD_DOWN, 100); break;
                case 2: press(pad, PAD_DOWN, 100); break;
                case 3: press(pad, PAD_LEFT, 100); press(pad, PAD_DOWN, 100); break;
                case 4: press(pad, PAD_LEFT, 100); break;
                case 5: press(pad, PAD_LEFT, 100); press(pad, PAD_UP, 100); break;
                case 6: press(pad, PAD_UP, 100); break;
                default: press(pad, PAD_RIGHT, 100); press(pad, PAD_UP, 100); break;
                }
                break;
            }
            case VPAD_FACE:
                if (f->sub >= 0 && f->sub < 4) {
                    press(pad, FACE[f->sub].slot, 100);
                }
                break;
            case VPAD_L3:     press(pad, PAD_LS, 100); break;
            case VPAD_R3:     press(pad, PAD_RS, 100); break;
            case VPAD_LB:     press(pad, PAD_LB, 100); break;
            case VPAD_LT:     press(pad, PAD_LT, 100); break;
            case VPAD_RB:     press(pad, PAD_RB, 100); break;
            case VPAD_RT:     press(pad, PAD_RT, 100); break;
            case VPAD_SELECT: press(pad, PAD_BACK, 100); break;
            case VPAD_START:  press(pad, PAD_START, 100); break;
            case VPAD_GUIDE:  press(pad, PAD_GUIDE, 100); break;
            default: break;
        }
    }
    VPAD_UNLOCK();
}

static void draw_label(SDL_Renderer *r, const char *text, float cx, float cy, SDL_Color colour) {
    if (!text) {
        return;
    }
    const int w = ui_text_width(UI_FONT_SMALL, text);
    ui_text(r, UI_FONT_SMALL, (int)cx - w / 2, (int)cy - ui_line_height(UI_FONT_SMALL) / 2,
            colour, text);
}

/* How hard this control is being pressed, from the pad state -- which by
 * the time it gets here holds the physical controller and the glass
 * merged together.
 *
 * Drawn from that rather than from which fingers are down, so the
 * overlay shows what is actually happening: press a shoulder button on a
 * Joy-Con and the one on screen lights up too. The browser page does the
 * same, and it is the difference between an overlay and a picture of
 * one. */
static int anchor_pressed(const PadState21 pad, int anchor, int sub) {
    switch (anchor) {
        case VPAD_FACE:  return (sub >= 0 && sub < 4) ? pad[FACE[sub].slot] : 0;
        case VPAD_L3:    return pad[PAD_LS];
        case VPAD_R3:    return pad[PAD_RS];
        case VPAD_LB:    return pad[PAD_LB];
        case VPAD_LT:    return pad[PAD_LT];
        case VPAD_RB:    return pad[PAD_RB];
        case VPAD_RT:    return pad[PAD_RT];
        case VPAD_SELECT: return pad[PAD_BACK];
        case VPAD_START: return pad[PAD_START];
        case VPAD_GUIDE: return pad[PAD_GUIDE];
        default:         return 0;
    }
}

void vpad_draw(SDL_Renderer *r, const PadState21 pad) {
    if (!g_enabled) {
        return;
    }

    for (int a = 0; a < VPAD_COUNT; a++) {
        if (!shown(a)) continue;
        float cx, cy;
        anchor_centre(a, &cx, &cy);
        const VpadStyle sv = style_of(a); const VpadStyle *st = &sv;
        const SDL_Color edge = g_editing ? COL_EDIT : COL_EDGE;
        const int on = !g_editing && anchor_pressed(pad, a, -1);

        switch (st->shape) {
            case SHAPE_PILL:
                ui_fill(r, (int)(cx - st->w / 2), (int)(cy - st->h / 2), (int)st->w, (int)st->h,
                        on ? COL_ON : COL_BODY);
                ui_outline(r, (int)(cx - st->w / 2), (int)(cy - st->h / 2), (int)st->w, (int)st->h,
                           2, edge);
                draw_label(r, st->label, cx, cy, label_colour());
                break;

            case SHAPE_CLUSTER:
                for (int i = 0; i < 4; i++) {
                    const float bx = cx + FACE[i].dx, by = cy + FACE[i].dy;
                    const int lit = !g_editing && anchor_pressed(pad, a, i);
                    ui_fill_circle(r, (int)bx, (int)by, (int)FACE_RADIUS, lit ? COL_ON : COL_BODY);
                    ui_draw_circle(r, (int)bx, (int)by, (int)FACE_RADIUS, edge);
                    draw_label(r, FACE[i].label, bx, by, label_colour());
                }
                if (g_editing) {
                    ui_draw_circle(r, (int)cx, (int)cy, (int)st->radius, COL_EDIT);
                }
                break;

            case SHAPE_CIRCLE:
            default:
                ui_fill_circle(r, (int)cx, (int)cy, (int)st->radius, COL_BODY);
                ui_draw_circle(r, (int)cx, (int)cy, (int)st->radius, edge);

                if (a == VPAD_LSTICK || a == VPAD_RSTICK) {
                    /* The knob sits where the stick is -- whichever
                     * stick moved it. */
                    const int vx = (a == VPAD_LSTICK) ? pad[PAD_LX] : pad[PAD_RX];
                    const int vy = (a == VPAD_LSTICK) ? pad[PAD_LY] : pad[PAD_RY];
                    const float kx = cx + (g_editing ? 0.0f : vx * STICK_TRAVEL / 100.0f);
                    /* Back into screen space for drawing, the same
                     * negation the finger went through on the way in. */
                    const float ky = cy - (g_editing ? 0.0f : vy * STICK_TRAVEL / 100.0f);
                    ui_fill_circle(r, (int)kx, (int)ky, (int)(0.8f * U), COL_KNOB);
                    ui_draw_circle(r, (int)kx, (int)ky, (int)(0.8f * U), edge);
                } else if (a == VPAD_DPAD) {
                    const float d = 1.05f * U, s = 0.34f * U;
                    const struct { int slot; float dx, dy; } arrows[4] = {
                        {PAD_UP, 0, -d}, {PAD_DOWN, 0, d}, {PAD_LEFT, -d, 0}, {PAD_RIGHT, d, 0},
                    };
                    for (int i = 0; i < 4; i++) {
                        const int lit = !g_editing && pad[arrows[i].slot];
                        ui_fill(r, (int)(cx + arrows[i].dx - s), (int)(cy + arrows[i].dy - s),
                                (int)(s * 2), (int)(s * 2), lit ? COL_ON : COL_KNOB);
                    }
                } else {
                    if (on) {
                        ui_fill_circle(r, (int)cx, (int)cy, (int)st->radius, COL_ON);
                        ui_draw_circle(r, (int)cx, (int)cy, (int)st->radius, edge);
                    }
                    draw_label(r, st->label, cx, cy, label_colour());
                }
                break;
        }
    }
}

void vpad_layout_to_string(char *out, size_t out_size) {
    size_t used = 0;
    out[0] = '\0';
    for (int a = 0; a < VPAD_COUNT; a++) {
        char one[48];
        int n = snprintf(one, sizeof(one), "%s%d:%.4f,%.4f", a ? ";" : "", a, g_pos[a].x, g_pos[a].y);
        if (n < 0 || used + (size_t)n + 1 >= out_size) {
            break;
        }
        memcpy(out + used, one, (size_t)n + 1);
        used += (size_t)n;
    }
}

void vpad_layout_from_string(const char *text) {
    if (!text || !*text) {
        return;
    }
    g_custom = 1;
    const char *p = text;
    while (*p) {
        int a = 0;
        float x = 0, y = 0;
        if (sscanf(p, "%d:%f,%f", &a, &x, &y) == 3 && a >= 0 && a < VPAD_COUNT) {
            /* Clamped: a hand-edited file must not put a control where
             * no finger can reach it. */
            g_pos[a].x = (x < 0.02f) ? 0.02f : (x > 0.98f ? 0.98f : x);
            g_pos[a].y = (y < 0.02f) ? 0.02f : (y > 0.98f ? 0.98f : y);
            g_revision++;
        }
        const char *sep = strchr(p, ';');
        if (!sep) {
            break;
        }
        p = sep + 1;
    }
}
