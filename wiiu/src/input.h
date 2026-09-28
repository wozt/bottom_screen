#ifndef BS_WIIU_INPUT_H
#define BS_WIIU_INPUT_H

#include <stddef.h>

int input_init(char *why, size_t why_size);
void input_exit(void);

/*
 * Samples the physical Wii U GamePad and sends only changed state.
 *
 * forward == 0 sends/releases a neutral pad, used while the local menu
 * owns the controls.
 */
void input_update(int forward);

/*
 * L3 + R3 is local to this client: one press switches between the bottom and
 * top streams.  The two stick clicks are not forwarded as remote buttons.
 * Returns 1 once per chord press, then 0 until both buttons have been released
 * and pressed together again.
 */
int input_take_screen_toggle(void);

#endif
