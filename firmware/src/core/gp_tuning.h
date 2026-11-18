/*
 * gp_tuning.h: the active profile's stick and trigger shaping, applied to
 * what the board reads (any mode, any transport):
 *  - sticks, radially: inner dead zone, outer dead zone (full output this
 *    far before the edge), anti-dead zone (output starts here once past the
 *    dead zone, for games with a dead zone of their own), response curve
 *    (linear, ^1.5, ^2, ^3: finer control near the centre);
 *  - triggers: dead zone, outer dead zone, hair trigger (any press past the
 *    dead zone is full).
 * The defaults (4 % inner dead zone, the rest off) are the shaping the pad
 * had before these settings.
 */
#ifndef GP_TUNING_H
#define GP_TUNING_H

#include "typedef.h"

void gp_tune_stick(s16 *x, s16 *y);     /* -32767..32767 each, unshaped in, shaped out */
void gp_tune_trigger(s16 *t);           /* -32768 released .. 32767 full */

#endif
