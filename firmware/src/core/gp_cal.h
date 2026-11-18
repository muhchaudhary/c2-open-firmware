/*
 * gp_cal.h: stick, trigger and gyro calibration (docs/config-protocol.md,
 * ops 0x10..0x15), stored (gp_store.h: syscfg item 48, or the C2's EEPROM) and applied at start-up.
 *
 * Without a stored calibration the drivers learn at runtime, as before:
 * stick centres over the first ~2.5 s, trigger rests over 0.5 s, the gyro
 * bias whenever the pad is still. With one, sticks and triggers start from
 * the stored centres, rests and full travel (no learning wait, no "don't
 * touch the sticks at boot"); the gyro starts from the stored bias and
 * still follows drift.
 *
 * The procedure, driven over the protocol:
 *  1. START (mask): REST, ~1 s with the sticks centred, the triggers
 *     released and the pad lying still (restarted if anything moves):
 *     stick centres, trigger rests, gyro bias.
 *  2. RANGE (if sticks or triggers are in the mask): every stick
 *     direction moved to its edge, the triggers pressed fully; furthest
 *     travel kept.
 *  3. FINISH: checked (every stick direction >= GP_CAL_STICK_MIN, each
 *     trigger >= GP_CAL_TRIG_MIN; a part short of it is named and the
 *     procedure goes on), stored, applied. CANCEL drops it.
 */
#ifndef GP_CAL_H
#define GP_CAL_H

#include "typedef.h"

enum { GP_CAL_IDLE, GP_CAL_REST, GP_CAL_RANGE, GP_CAL_DONE };

void gp_cal_load(void);                 /* start-up, after the board's drivers: the stored calibration */
void gp_cal_tick(void);                 /* tick, after gp_board_read: sampling while calibrating */
/* Protocol ops (poll context): payload in a / alen, reply payload into out.
 * Return a gp_config status. */
u8 gp_cal_op(u8 op, const u8 *a, int alen, u8 *out);

#endif
