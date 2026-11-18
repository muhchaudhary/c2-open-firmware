/* gp_motion.c: see gp_motion.h. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_log.h"
#include "gp_motion.h"

/* Still: a window of STILL_MS in two halves. Measured on the C2 lying on a
 * desk (2026-11-17, 1.5 s windows, raw units): gyro max - min 3-6 per axis,
 * accel max - min 15-24, the accel's half-window means 0.1-2.1 apart. */
#define STILL_MS            1500
#define STILL_GAP_MS        100             /* no sample this long: start again */
#define GYRO_BAND           24              /* max - min per axis (1.5 dps at 16 LSB/dps) */
#define ACC_BAND            100             /* max - min per axis (12 mg at 8192 LSB/g): resting, not held */
#define ACC_DRIFT           12              /* the halves' means: a slow tilt (~0.1 dps) moves them */
/* What a learned bias may be. A turn about the vertical axis leaves the
 * accelerometer as it is, so this is the only guard against a slow steady
 * one: the first bias (none stored) under BIAS_MAX, later ones within
 * BIAS_NEAR of the current one (temperature drift is slow); else only the
 * same value again for BIAS_REPEAT windows in a row (a real change
 * persists, a turn while resting doesn't). This C2: 51 -41 -4 raw (3.2 / 2.6 dps). */
#define BIAS_MAX            160             /* 10 dps */
#define BIAS_NEAR           24              /* 1.5 dps */
#define BIAS_REPEAT         4               /* windows: 6 s */

s16 gp_motion[6];
s16 gp_motion_gyro_raw[3];
static s16 bias[3];
static u8 has_bias;                         /* stored or learned: new ones must be near it */
static s16 cand[3];                         /* a refused one, and how many windows in a row gave it */
static u8 cand_n;

static struct {
    u8 on, half;
    u32 t0, last;
    u16 n[2];
    s32 acc[2][3];
    s32 gyr[3];
    s16 gmin[3], gmax[3], amin[3], amax[3];
} w;

void gp_motion_set_bias(const s16 b[3])
{
    for (int i = 0; i < 3; i++) {
        bias[i] = b[i];
    }
    has_bias = 1;
    cand_n = 0;
}

static void still_start(u32 now, const s16 *a, const s16 *g)
{
    memset(&w, 0, sizeof(w));
    w.on = 1;
    w.t0 = w.last = now;
    for (int i = 0; i < 3; i++) {
        w.gmin[i] = w.gmax[i] = g[i];
        w.amin[i] = w.amax[i] = a[i];
    }
}

static int band(s16 *lo, s16 *hi, s16 v, int max)
{
    if (v < *lo) {
        *lo = v;
    }
    if (v > *hi) {
        *hi = v;
    }
    return *hi - *lo <= max;
}

static int absi(int v)
{
    return v < 0 ? -v : v;
}

/* The window is over: take its gyro mean as the bias if it passes. */
static void still_done(void)
{
    if (!w.n[0] || !w.n[1]) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (absi(w.acc[1][i] / w.n[1] - w.acc[0][i] / w.n[0]) > ACC_DRIFT) {
            return;                                      /* tilting slowly */
        }
    }
    s16 b[3];
    int n = w.n[0] + w.n[1];
    int far = 0, again = cand_n > 0;
    for (int i = 0; i < 3; i++) {
        b[i] = w.gyr[i] / n;
        far |= has_bias ? absi(b[i] - bias[i]) > BIAS_NEAR : absi(b[i]) > BIAS_MAX;
        again &= absi(b[i] - cand[i]) <= BIAS_NEAR;
    }
    if (far) {
        cand_n = again ? cand_n + 1 : 1;
        if (!again) {
            memcpy(cand, b, sizeof(cand));
        }
        if (cand_n == 1) {
            gp_log("motion: still, but gyro %d %d %d is far from the bias %d %d %d (a slow turn?)",
                   b[0], b[1], b[2], bias[0], bias[1], bias[2]);
        }
        if (cand_n < BIAS_REPEAT) {
            return;
        }
        gp_log("motion: the same %u windows in a row: taken", BIAS_REPEAT);
    }
    cand_n = 0;
    if (!has_bias || absi(b[0] - bias[0]) + absi(b[1] - bias[1]) + absi(b[2] - bias[2]) >= 4) {
        gp_log("motion: gyro bias %d %d %d -> %d %d %d", bias[0], bias[1], bias[2], b[0], b[1], b[2]);
    }
    memcpy(bias, b, sizeof(bias));
    has_bias = 1;
}

static void learn_bias(const s16 *a, const s16 *g)
{
    u32 now = gp_ms();
    if (!w.on || now - w.last > STILL_GAP_MS) {
        still_start(now, a, g);
    }
    w.last = now;
    for (int i = 0; i < 3; i++) {
        if (!band(&w.gmin[i], &w.gmax[i], g[i], GYRO_BAND) || !band(&w.amin[i], &w.amax[i], a[i], ACC_BAND)) {
            still_start(now, a, g);                      /* moved: start over from this sample */
            break;
        }
    }
    for (int i = 0; i < 3; i++) {
        w.gyr[i] += g[i];
        w.acc[w.half][i] += a[i];
    }
    w.n[w.half]++;
    if (w.half == 0 && now - w.t0 >= STILL_MS / 2) {
        w.half = 1;
    } else if (w.half == 1 && now - w.t0 >= STILL_MS) {
        still_done();
        w.on = 0;                                        /* the next sample starts a new window */
    }
}

void gp_motion_update(const s16 acc[3], const s16 gyr[3])
{
    learn_bias(acc, gyr);
    for (int i = 0; i < 3; i++) {
        gp_motion_gyro_raw[i] = gyr[i];
        gp_motion[i] = acc[i];
        s32 v = gyr[i] - bias[i];
        gp_motion[3 + i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
}
