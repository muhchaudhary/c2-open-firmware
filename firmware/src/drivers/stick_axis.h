/*
 * stick_axis.h: one analog stick axis, from raw sensor values to
 * -32767..32767. Generic: any source of raw values (a stick chip, the ADC).
 *
 * As stock's stick_process (C2 0x1E49332): a mean of cfg->avg reads, per-direction
 * scaling with a margin (full output a little before the edge), then a
 * radial dead-zone on the stick's radius (stick_axis_radial) rescaled to full
 * and clamped to a circle. Centre: averaged over 1 s once values have been
 * arriving for 1.5 s, restarted if the stick moves meanwhile; slow
 * re-centring near the centre afterwards. Scale: learned per direction,
 * starting from span0. A click held up or down pushes the magnet far past
 * the stick's travel (LY: ~3500 raw, to the chip's limit), before the
 * switch closes and after it opens too: a new extreme is only pending
 * until STICK_COMMIT_MS have passed with no click (stick_axis_hold), and
 * nothing is learned until STICK_HOLD_MS after one. Calibrated, the stored
 * spans are kept. Call stick_axis_read once per report (durations go by
 * the clock, gp_ms, not by the number of calls).
 */
#ifndef STICK_AXIS_H
#define STICK_AXIS_H

#include "typedef.h"

#define STICK_AXIS_AVG      6

enum { STICK_NEW, STICK_SETTLING, STICK_LEARNING, STICK_READY };   /* the centre */

struct stick_axis_cfg {
    s32 span0;                          /* initial half-range, raw units */
    s32 margin;                         /* full output this far before the furthest travel seen */
    s32 lo, hi;                         /* plausible raw range while learning the centre */
    s32 still;                          /* max movement while learning the centre */
    s32 recentre;                       /* re-centre only this close to the centre */
    u8 avg;                             /* reads averaged, 1..STICK_AXIS_AVG (1 or 0: none) */
};

struct stick_axis {
    const char *name;
    const struct stick_axis_cfg *cfg;
    u8 phase;                           /* STICK_*: learning the centre */
    u32 t0;                             /* when the phase (or the average) started */
    u32 rest_sum;
    u16 rest_n;                         /* samples averaged */
    s32 centre;
    s32 centre_frac;
    s32 span_pos, span_neg;
    u16 hist[STICK_AXIS_AVG];
    u16 filt;                           /* the last raw value after the average (the probe) */
    u8 hist_i, hist_n;
    u16 restarts;                       /* learning restarted (moved) */
    u8 odd;                             /* consecutive readings off the centre while learning */
    u8 cal;                             /* calibrated: centre and spans stored, no learning */
    u8 hold;                            /* no span learning until STICK_HOLD_MS after hold_t0 */
    u32 hold_t0;                        /* the last stick_axis_hold */
    s32 pend_pos, pend_neg;             /* a new extreme each way, not committed yet (0: none) */
    u32 pend_t0;                        /* when the first pending one came */
    s32 cal_centre;
};

void stick_axis_init(struct stick_axis *a, const char *name, const struct stick_axis_cfg *cfg);
void stick_axis_relearn(struct stick_axis *a);           /* learn the centre again (calibrated: back to the stored one) */
/* Stored calibration: the centre and the travel each way (raw units, the
 * margin still applies); no learning at start. clear: learn again. */
void stick_axis_set_cal(struct stick_axis *a, s32 centre, s32 span_pos, s32 span_neg);
void stick_axis_clear_cal(struct stick_axis *a);
int  stick_axis_read(struct stick_axis *a, s32 raw);     /* -32767..32767, 0 while learning */
/* The stick is clicked (call each tick while it is): pending extremes are
 * dropped, and nothing is learned until STICK_HOLD_MS after the last call. */
void stick_axis_hold(struct stick_axis *a);
/* One stick: radial dead-zone (inner, of 32767), rescaled to full, clamped to a circle. */
void stick_axis_radial(s32 x, s32 y, s32 inner, int *ox, int *oy);

#endif
