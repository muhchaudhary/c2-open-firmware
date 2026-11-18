/* hall_trigger.c: see hall_trigger.h. */
#include "system/includes.h"
#include "asm/adc_api.h"
#include "gamepad.h"
#include "gp_log.h"
#include "hall_trigger.h"

#define TRIG_REST_MS        500         /* the rest: averaged this long */
#define TRIG_DEADZONE       12          /* ADC counts (10-bit): charging shifts the rest by ~6 */
#define TRIG_REZERO         50          /* ADC counts below the rest a stopper may hold the trigger (C2: 22-28) */
#define TRIG_STILL          3           /* ADC counts of wobble that still count as held still */
#define TRIG_REZERO_MS      3000

void hall_trigger_init(struct hall_trigger *t)
{
    t->span = t->span_min;
    adc_add_sample_ch(t->adc_ch);
}

void hall_trigger_set_cal(struct hall_trigger *t, u16 rest, u16 span)
{
    t->rest = rest;
    t->span = span;
    t->rest_done = 1;                                    /* nothing to learn */
    t->still_t0 = gp_ms();
}

void hall_trigger_clear_cal(struct hall_trigger *t)
{
    t->rest_done = 0;
    t->rest_n = 0;
    t->rest_sum = 0;
    t->span = t->span_min;
}

int hall_trigger_read(struct hall_trigger *t, int endstop)
{
    /* idx stays inside the ring and `filled` counts up once. (idx used to
     * run free as a u8 and stand in for "history full": it wrapped every
     * 256 reads, ~1 s, and both triggers read released for the next 8
     * reports, seen at 250 reports/s through the receiver, 2026-11-13.) */
    t->hist[t->idx] = adc_get_value(t->adc_ch);
    t->idx = (t->idx + 1) % HALL_TRIGGER_AVG;
    if (t->filled < HALL_TRIGGER_AVG) {
        t->filled++;
    }
    u32 sum = 0;
    for (int i = 0; i < HALL_TRIGGER_AVG; i++) {
        sum += t->hist[i];
    }
    u16 avg = sum / HALL_TRIGGER_AVG;
    t->raw = avg;
    u32 now = gp_ms();
    if (!t->rest_done) {                                 /* learning the rest position */
        if (t->filled >= HALL_TRIGGER_AVG) {             /* history filled */
            if (!t->rest_n) {
                t->rest_t0 = now;
            }
            t->rest_sum += avg;
            t->rest_n++;
            t->rest = t->rest_sum / t->rest_n;
            t->rest_done = now - t->rest_t0 >= TRIG_REST_MS;
        }
        return -32768;
    }
    if (t->filled < HALL_TRIGGER_AVG) {
        /* Calibrated from the start: the history isn't full yet, and its
         * zeros would read as a full press (and grow the full scale). */
        return -32768;
    }
    /* Pressing lowers the value (C2: ~485 at rest, ~11 at full). The rest
     * moves with the trigger stoppers: the stopper holds the trigger a
     * little in. Above the learned rest (stopper taken off after boot): that
     * is the rest now. A little below it and held still for TRIG_REZERO_MS
     * (stopper put on after boot): that is the rest now. (2026-11-13: either
     * way round read as a slight press.) */
    int travel = (int)t->rest - avg;
    if (travel < 0) {
        if (-travel > TRIG_DEADZONE / 2) {
            gp_log("trigger %s: rest %u -> %u (released further)", t->name, t->rest, avg);
            t->rest = avg;
        }
        travel = 0;
    }
    if (travel > TRIG_DEADZONE && travel <= TRIG_REZERO && !endstop) {
        int d = (int)avg - t->still_ref;
        if (d < -TRIG_STILL || d > TRIG_STILL) {
            t->still_ref = avg;
            t->still_t0 = now;
        } else if (now - t->still_t0 >= TRIG_REZERO_MS) {
            gp_log("trigger %s: rest %u -> %u (held still %u ms)", t->name, t->rest, avg, TRIG_REZERO_MS);
            t->rest = avg;
            t->still_t0 = now;
            travel = 0;
        }
    } else {
        t->still_t0 = now;
        t->still_ref = avg;
    }
    if (travel > t->span || (endstop && travel > TRIG_DEADZONE * 4)) {
        t->span = travel;                                /* full scale learned */
    }
    if (endstop) {
        return 32767;
    }
    if (travel <= TRIG_DEADZONE) {
        return -32768;
    }
    int v = (travel - TRIG_DEADZONE) * 65535 / (t->span - TRIG_DEADZONE);
    return (v > 65535 ? 65535 : v) - 32768;
}
