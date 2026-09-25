#ifndef BS_WIIU_KEYBOARD_H
#define BS_WIIU_KEYBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The console's own on-screen keyboard, for typing into a menu field.
 *
 * It is nn::swkbd, which is C++ -- hence a .cpp behind this header --
 * and it DRAWS ITSELF WITH GX2. The menu around it is OSScreen, and the
 * two cannot be on screen at once: OSScreen and GX2 both want the scan
 * buffers. So this call takes the screen for the duration, runs the
 * keyboard's own loop, and hands the display back the way it found it.
 *
 * That is why it blocks rather than being a state in the main loop: for
 * as long as it runs, the program's own drawing must not happen.
 *
 * `initial` prefills the field and may be NULL. `numeric` asks for the
 * numeric pad rather than the full keyboard -- an address and a port
 * are digits and dots. Returns 1 when the user accepted, 0 when they
 * cancelled, and -1 when the keyboard could not be opened at all, with
 * the reason in `why`.
 *
 * The implementation restores SDL's cached GX2 state before returning.
 */
int keyboard_prompt(const char *hint, const char *initial, int numeric,
                    char *out, size_t out_size, char *why, size_t why_size);

#ifdef __cplusplus
}
#endif

#endif /* BS_WIIU_KEYBOARD_H */
