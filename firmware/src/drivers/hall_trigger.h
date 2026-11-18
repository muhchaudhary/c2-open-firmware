/*
 * hall_trigger.h: an analog trigger read from a hall sensor on the ADC
 * (10-bit; the value falls as the trigger is pressed), with an optional
 * end-stop switch. Self-calibrating: the rest position is the average of the
 * first 0.5 s and then follows trigger stoppers, travel is measured from it,
 * full scale grows to the largest travel seen; at the end-stop the trigger is
 * full and that travel becomes full scale. Call hall_trigger_read once per
 * report (durations go by the clock, gp_ms, not by the number of calls).
 */
#ifndef HALL_TRIGGER_H
#define HALL_TRIGGER_H

#include "typedef.h"

#define HALL_TRIGGER_AVG    8           /* samples, one per tick */

struct hall_trigger {
    /* configuration */
    const char *name;
    u32 adc_ch;                         /* AD_CH_* */
    u16 span_min;                       /* initial full scale, ADC counts */
    /* state */
    u16 hist[HALL_TRIGGER_AVG];
    u8  idx;                            /* next slot in hist, 0..HALL_TRIGGER_AVG-1 */
    u8  filled;                         /* samples in hist, up to HALL_TRIGGER_AVG (then stays) */
    u32 rest_sum;
    u16 rest_n;                         /* samples averaged, since rest_t0 */
    u32 rest_t0;
    u8  rest_done;                      /* the rest is learned (or calibrated) */
    u16 rest;
    u16 span;
    u16 raw;                            /* last averaged value, for debugging */
    u16 still_ref;                      /* re-zeroing: where it sits ... */
    u32 still_t0;                       /* ... since when */
};

void hall_trigger_init(struct hall_trigger *t);
/* Stored calibration: the rest position and the full travel (ADC counts);
 * no learning at start (the stopper handling still moves the rest). clear:
 * learn again. */
void hall_trigger_set_cal(struct hall_trigger *t, u16 rest, u16 span);
void hall_trigger_clear_cal(struct hall_trigger *t);
/* -32768 (released) .. 32767 (full); endstop: the end-stop switch is closed */
int  hall_trigger_read(struct hall_trigger *t, int endstop);

#endif
