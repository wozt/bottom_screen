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

#endif
