/* stick_axis.c: see stick_axis.h. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_log.h"
#include "stick_axis.h"

#define STICK_SETTLE_MS     1500                  /* data arriving this long before learning */
#define STICK_LEARN_MS      1000                  /* the centre: averaged this long */
#define STICK_HOLD_MS       200                   /* after a click: the stick still past its travel */
#define STICK_COMMIT_MS     200                   /* a new extreme with no click this long: the stick's */

void stick_axis_init(struct stick_axis *a, const char *name, const struct stick_axis_cfg *cfg)
{
    memset(a, 0, sizeof(*a));
    a->name = name;
    a->cfg = cfg;
    a->span_pos = a->span_neg = cfg->span0;
}

void stick_axis_relearn(struct stick_axis *a)
{
    if (a->cal) {
        a->centre = a->cal_centre;                       /* calibrated: nothing to learn */
        a->centre_frac = 0;
        a->hist_n = 0;
        return;
    }
    a->phase = STICK_NEW;
    a->rest_n = 0;
    a->rest_sum = 0;
}

void stick_axis_set_cal(struct stick_axis *a, s32 centre, s32 span_pos, s32 span_neg)
{
    a->cal = 1;
    a->cal_centre = centre;
    a->centre = centre;
    a->centre_frac = 0;
    a->span_pos = span_pos;
    a->span_neg = span_neg;
    a->phase = STICK_READY;                              /* nothing to learn */
    a->hist_n = 0;
    a->pend_pos = a->pend_neg = 0;
}

void stick_axis_clear_cal(struct stick_axis *a)
{
    a->cal = 0;
    a->span_pos = a->span_neg = a->cfg->span0;
    a->pend_pos = a->pend_neg = 0;
    a->hist_n = 0;
    stick_axis_relearn(a);
}

void stick_axis_hold(struct stick_axis *a)
{
    a->hold = 1;
    a->hold_t0 = gp_ms();
    a->pend_pos = a->pend_neg = 0;                       /* read on the way into the click */
}

/* Learning the spans (not calibrated): a reading past one is pending; it
 * becomes the span after STICK_COMMIT_MS without a click. */
static void stick_axis_learn(struct stick_axis *a, s32 d, u32 now)
{
    if (a->hold) {
        if (now - a->hold_t0 < STICK_HOLD_MS) {
            return;
        }
        a->hold = 0;
    }
    int pending = a->pend_pos || a->pend_neg;
    if (d > a->span_pos && d > a->pend_pos) {
        a->pend_pos = d;
    } else if (-d > a->span_neg && -d > a->pend_neg) {
        a->pend_neg = -d;
    }
    if (!a->pend_pos && !a->pend_neg) {
        return;
    }
    if (!pending) {
        a->pend_t0 = now;                                /* the first pending one */
    }
    if (now - a->pend_t0 < STICK_COMMIT_MS) {
        return;
    }
    if (a->pend_pos > a->span_pos) {
        a->span_pos = a->pend_pos;
    }
    if (a->pend_neg > a->span_neg) {
        a->span_neg = a->pend_neg;
    }
    a->pend_pos = a->pend_neg = 0;
}

int stick_axis_read(struct stick_axis *a, s32 raw)
{
    const struct stick_axis_cfg *c = a->cfg;
    u32 now = gp_ms();
    if (a->phase != STICK_READY) {                       /* settling, then learning the centre */
        if (raw <= c->lo || raw >= c->hi) {              /* implausible: skip */
            return 0;
        }
        if (a->phase == STICK_NEW) {
            a->phase = STICK_SETTLING;
            a->t0 = now;
        }
        if (a->phase == STICK_SETTLING) {
            a->centre = raw;
            if (now - a->t0 >= STICK_SETTLE_MS) {
                a->phase = STICK_LEARNING;
                a->t0 = now;
                a->rest_n = 0;
                a->rest_sum = 0;
            }
            return 0;
        }
        s32 dev = raw - a->centre;
        if (a->rest_n > 0 && (dev > c->still || dev < -c->still)) {
            /* A lone odd reading (the C2's stick chip runs 10-15 % bad frames
             * in its first seconds) is skipped; only 3 in a row mean the
             * stick really moved: start the average again. (2026-11-13: one
             * boot restarted ~60 times and the sticks came alive only at
             * 10 s.) */
            if (++a->odd < 3) {
                return 0;
            }
            a->odd = 0;
            a->restarts++;
            a->t0 = now;
            a->rest_n = 0;
            a->rest_sum = 0;
            a->centre = raw;
            return 0;
        }
        a->odd = 0;
        a->rest_sum += raw;
        a->rest_n++;
        a->centre = a->rest_sum / a->rest_n;
        if (now - a->t0 >= STICK_LEARN_MS) {
            a->phase = STICK_READY;
            gp_log("sticks: %s centre %d learned (%u restarts)", a->name, (int)a->centre, a->restarts);
        }
        return 0;
    }
    /* A mean of cfg->avg reads (stock: 6). Taken at the report rate, each
     * read more adds half a tick of lag on average (6 at a ~4 ms tick:
     * ~10 ms); a source that streams faster than the reports is better
     * averaged at the source (hjc_stick). */
    int avg = c->avg > STICK_AXIS_AVG ? STICK_AXIS_AVG : c->avg ? c->avg : 1;
    a->hist[a->hist_i] = raw;
    a->hist_i = (a->hist_i + 1) % avg;
    if (a->hist_n < avg) {
        a->hist_n++;
    }
    s32 sum = 0;
    for (int k = 0; k < a->hist_n; k++) {
        sum += a->hist[k];
    }
    raw = sum / a->hist_n;
    a->filt = raw;

    s32 d = raw - a->centre;
    /* Slow re-centring while the stick is near the centre: corrects a
     * centre learned off by a little, or drift (~8 s time constant). */
    if (d > -c->recentre && d < c->recentre) {
        a->centre_frac += d;
        a->centre += a->centre_frac / 1024;
        a->centre_frac %= 1024;
        d = raw - a->centre;
    }
    /* The furthest travel seen, each way (stick_axis_learn): not when
     * calibrated (the stored spans are the stick's), nor from a click (it
     * read past the travel, and that direction never reached full again
     * until a restart, 2026-11-14). Past the span: clamped to full. */
    if (!a->cal) {
        stick_axis_learn(a, d, now);
    }
    s32 v;
    if (d >= 0) {
        v = d * 32767 / (a->span_pos - c->margin);
    } else {
        v = d * 32767 / (a->span_neg - c->margin);
    }
    return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
}

static u32 isqrt(u32 n)
{
    u32 r = 0, bit = 1u << 30;
    while (bit > n) {
        bit >>= 2;
    }
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

void stick_axis_radial(s32 x, s32 y, s32 inner, int *ox, int *oy)
{
    u32 r = isqrt((u32)(x * x) + (u32)(y * y));
    if (r <= inner) {
        *ox = *oy = 0;
        return;
    }
    u32 rr = (r - inner) * 32767 / (32767 - inner);
    if (rr > 32767) {
        rr = 32767;
    }
    *ox = x * (s32)rr / (s32)r;
    *oy = y * (s32)rr / (s32)r;
}
