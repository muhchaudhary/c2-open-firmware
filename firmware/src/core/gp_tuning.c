/* gp_tuning.c: see gp_tuning.h. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_settings.h"
#include "gp_tuning.h"

#define FULL                32767
#define HAIR_MIN            (65535u * 10 / 100)   /* the hair trigger fires past 10 % at least */

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

/* t in 0..FULL through the curve (Q15). */
static u32 curve(u32 t, u8 kind)
{
    if (t >= FULL) {
        return FULL;                                     /* Q15 rounds down: full stays full (32767, not 32765) */
    }
    switch (kind) {
    case 1:
        return t * isqrt(t << 15) >> 15;                 /* t^1.5 */
    case 2:
        return t * t >> 15;                              /* t^2 */
    case 3:
        return (t * t >> 15) * t >> 15;                  /* t^3 */
    default:
        return t;
    }
}

void gp_tune_stick(s16 *x, s16 *y)
{
    const struct gp_profile *p = &GP_ACTIVE_PROFILE;
    s32 sx = *x, sy = *y;
    u32 r = isqrt((u32)(sx * sx) + (u32)(sy * sy));
    u32 inner = p->stick_deadzone * FULL / 100;
    u32 end = (100 - p->stick_outer) * FULL / 100;
    if (r <= inner || r == 0) {
        *x = *y = 0;
        return;
    }
    u32 t = end > inner ? (r - inner) * FULL / (end - inner) : FULL;
    if (t > FULL) {
        t = FULL;
    }
    t = curve(t, p->stick_curve);
    u32 anti = p->stick_anti_deadzone * FULL / 100;
    u32 rr = anti + t * (FULL - anti) / FULL;
    if (rr > FULL) {
        rr = FULL;
    }
    *x = sx * (s32)rr / (s32)r;                          /* same direction, new radius (clamped to a circle) */
    *y = sy * (s32)rr / (s32)r;
}

void gp_tune_trigger(s16 *t)
{
    const struct gp_profile *p = &GP_ACTIVE_PROFILE;
    u32 v = (u32)(*t + 32768);                           /* 0 .. 65535 */
    u32 dz = p->trigger_deadzone * 65535u / 100;
    u32 end = (100 - p->trigger_outer) * 65535u / 100;
    if (p->hair_trigger) {
        /* At least HAIR_MIN of the travel: right above the driver's noise
         * floor a finger resting on the trigger (or charging's shift of the
         * rest) flicked it to full (2026-11-12). */
        u32 at = dz > HAIR_MIN ? dz : HAIR_MIN;
        *t = v > at ? 32767 : -32768;
        return;
    }
    if (v <= dz) {
        *t = -32768;
        return;
    }
    if (dz == 0 && end >= 65535) {
        return;                                          /* nothing to do (the default) */
    }
    u32 o = end > dz ? (v - dz) * 65535u / (end - dz) : 65535u;
    if (o > 65535) {
        o = 65535;
    }
    *t = (s32)o - 32768;
}
