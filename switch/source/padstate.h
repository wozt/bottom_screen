#ifndef BS_SWITCH_PADSTATE_H
#define BS_SWITCH_PADSTATE_H

#include <stdint.h>

/*
 * The controller state the on-screen pad speaks.
 *
 * Taken as-is from capture2cloud, whose homebrew this pad came from, so
 * that vpad.c stays the file it already is rather than being rewritten
 * around a different vocabulary. Nothing else in this client uses it:
 * main.c translates between these slots and the protocol's own button
 * codes at the one boundary where the two meet.
 *
 * The names are an Xbox pad's, which is why PAD_A is the bottom button
 * and not the right one. The labels drawn on screen are Nintendo's, and
 * the face table in vpad.c is where the two are reconciled.
 */
#define PAD_SLOT_COUNT 21

enum {
    PAD_GUIDE = 0,
    PAD_BACK  = 1,
    PAD_START = 2,
    PAD_RB    = 3,
    PAD_RT    = 4,
    PAD_RS    = 5,
    PAD_LB    = 6,
    PAD_LT    = 7,
    PAD_LS    = 8,
    PAD_RX    = 9,
    PAD_RY    = 10,
    PAD_LX    = 11,
    PAD_LY    = 12,
    PAD_UP    = 13,
    PAD_DOWN  = 14,
    PAD_LEFT  = 15,
    PAD_RIGHT = 16,
    PAD_Y     = 17,
    PAD_B     = 18,
    PAD_A     = 19,
    PAD_X     = 20
};

/* A button is 0 or 100; a stick axis runs -100..100. */
typedef int8_t PadState21[PAD_SLOT_COUNT];

#endif
