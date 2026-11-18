/*
 * gp_power.h: turning off, as stock (C2 event dispatcher around 0x1E46D00,
 * its counters h[0xED92] / h[0xED94] count seconds):
 *  - not connected, pairing mode: off after GP_PAIR_OFF_S ("pair over time sleep");
 *  - not connected, otherwise (reconnecting / waiting): off after
 *    GP_RECONNECT_OFF_S ("reconnect over time sleep");
 *  - connected with nothing pressed or moved: off after
 *    gp_settings.idle_off_min (a setting, gp_config.h; default 10. Stock: a
 *    per-profile setting in minutes, 0 = never; GameSir Connect
 *    1.14.2 offers Never / 1 / 5 / 10 / 20 for its other pads, default 10).
 *    "Moved" = sticks or triggers off rest; the gyro doesn't count;
 *  - hold Home alone for GP_HOME_OFF_MS: the LEDs go dark, off on release;
 *  - low battery: the SDK's own check (get_vbat_level, patched to the
 *    board's battery pin) ends in gp_power_off too.
 * Wired (USB host): no idle-off, the host powers the pad. Off with USB 5 V
 * present is charging mode (gp_charging.h), as stock.
 */
#ifndef GP_POWER_H
#define GP_POWER_H

#include "typedef.h"

#define GP_PAIR_OFF_S       60
#define GP_RECONNECT_OFF_S  180
#define GP_HOME_OFF_MS      5000

void gp_power_off(const char *why);     /* poll context: closes the link first, then off */
void gp_power_off_lowbat(void);         /* the SDK's power event handler */
int  gp_power_off_pending(void);
void gp_power_off_step(void);           /* every GP_POLL_MS once gp_power_off ran */
void gp_power_poll(u8 connected, u8 wired);   /* every GP_POLL_MS; wired: no idle-off */
void gp_power_activity(void);           /* any context: sticks or triggers off rest */
void gp_power_restart_idle(void);       /* any context: a new state (pairing mode) starts its count */
int  gp_power_home_held(void);          /* Home held for off: the LEDs are dark */
void gp_power_soft_off(void);           /* off now (or charging mode with 5 V) */

#endif
