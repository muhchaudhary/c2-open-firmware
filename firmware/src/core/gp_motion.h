/*
 * gp_motion.h: motion data in SInput's frame, gyro bias removed. The bias
 * is learned whenever the pad rests (1.5 s with gyro and accelerometer each
 * within a small band, and no slow tilt), so it follows temperature drift
 * and needs no button (stock calibrates 130 samples on request and stores
 * them). A new bias must be near the current one (a stored one seeds it),
 * or the same for several windows in a row: a slow steady turn is not
 * bias. The accelerometer is reported as measured.
 */
#ifndef GP_MOTION_H
#define GP_MOTION_H

#include "typedef.h"

/* Report tick, with a new sample already in SInput's frame and units. */
void gp_motion_update(const s16 acc[3], const s16 gyr[3]);
extern s16 gp_motion[6];                /* accel X Y Z, gyro X Y Z: the last sample */
extern s16 gp_motion_gyro_raw[3];       /* the last gyro sample before the bias (calibration) */
void gp_motion_set_bias(const s16 b[3]);  /* a stored bias (calibration): the starting point */

/* Saturating negation, for axis mapping. */
static inline s16 gp_neg(s16 v)
{
    return v == -32768 ? 32767 : -v;
}

#endif
