/*
 * gp_rumble.h: rumble from the host, the intensity level (gp_settings) and
 * its hotkeys: Modifier + Up / Modifier + Down set the level (C2 stock
 * 0x1E49AF0: M + Up / Down, on the press), then 1 s of rumble at the new
 * level as feedback. The host's amplitudes are only stored (any context);
 * the report tick applies them, and stops the motors when the host goes.
 */
#ifndef GP_RUMBLE_H
#define GP_RUMBLE_H

#include "typedef.h"

void gp_rumble_init(void);
void gp_rumble_set(u8 left, u8 right);  /* from the host, 0..255 */
void gp_rumble_set_timed(u8 left, u8 right, u32 ms);   /* ... stopping after ms (0: no limit) */
void gp_rumble_tick(u32 buttons);       /* report tick: hotkeys, then the motors */
void gp_rumble_stop(void);              /* any context */

#endif
