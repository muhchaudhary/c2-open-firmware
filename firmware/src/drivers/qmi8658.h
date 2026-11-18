/*
 * qmi8658.h: QST QMI8658 6-axis IMU on the hardware IIC (jl_iic.h), set up
 * as stock GameSir firmware does: accel +-4 g (8192 LSB/g), gyro +-2048 dps
 * (16 LSB/dps), 448 Hz, low-pass on. Polled from the report tick.
 */
#ifndef QMI8658_H
#define QMI8658_H

#include "typedef.h"

void qmi8658_init(u8 addr);             /* soft reset; configured by the polls 50 ms later */
/* Report tick. 1 = a new sample in raw: AX AY AZ GX GY GZ in the sensor's frame. */
int  qmi8658_poll(s16 raw[6]);

/* Debug log counters. */
extern u32 qmi8658_ff;                  /* samples read again: all 0xFF */
extern u32 qmi8658_glitches;            /* implausible jumps that a re-read didn't confirm */

#endif
