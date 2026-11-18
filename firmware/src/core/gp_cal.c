/* gp_cal.c: see gp_cal.h. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_motion.h"
#include "gp_config.h"
#include "gp_cal.h"
#include "gp_log.h"
#include "gp_store.h"

#define GP_CAL_ID           GP_STORE_CAL
#define GP_CAL_MAGIC        0xCA
#define GP_CAL_VERSION      1
#define GP_CAL_ALL          (GP_CAL_STICKS | GP_CAL_TRIGGERS | GP_CAL_GYRO)

#define CAL_REST_MS         1000        /* held still this long */
#define CAL_STICK_BAND      600         /* raw, max - min while resting (the chip's noise ~+-50) */
#define CAL_TRIG_BAND       8           /* ADC counts */
#define CAL_GYRO_BAND       48          /* raw gyro (3 dps): resting in the hand is fine here */
#define GP_CAL_STICK_MIN    6000        /* raw travel each way (C2 full swing ~10000-12500) */
#define GP_CAL_TRIG_MIN     200         /* ADC counts (C2 full travel ~470) */

struct cal_record {
    u8 magic;
    u8 version;
    struct gp_cal cal;                  /* this build's layout; the version guards it */
};

static struct gp_cal cal_stored;

/* The procedure's state: written by the tick, read by the poll. */
static volatile u8 cal_phase, cal_mask;
static u16 cal_n, cal_restarts;         /* cal_n: samples since cal_t0 */
static u32 cal_t0;
static s32 st_sum[4], tr_sum[2], gy_sum[3];
static u16 st_lo[4], st_hi[4], tr_lo[2], tr_hi[2];
static s16 gy_lo[3], gy_hi[3];
static u16 st_centre[4], st_min[4], st_max[4], tr_rest[2], tr_min[2];
static s16 gy_bias[3];
static struct gp_raw cal_now;

void gp_cal_load(void)
{
    struct cal_record r;
    memset(&r, 0, sizeof(r));
    int n = gp_store_read(GP_CAL_ID, &r, sizeof(r));
    if (n != sizeof(r) || r.magic != GP_CAL_MAGIC || r.version != GP_CAL_VERSION) {
        gp_log("cal: none stored, learning at runtime");
        return;
    }
    cal_stored = r.cal;
    cal_stored.valid &= GP_CAL_ALL;
    gp_board_cal_apply(&cal_stored, 0);
    if (cal_stored.valid & GP_CAL_GYRO) {
        gp_motion_set_bias(cal_stored.gyro_bias);
    }
    gp_log("cal: stored%s%s%s", cal_stored.valid & GP_CAL_STICKS ? " sticks" : "",
           cal_stored.valid & GP_CAL_TRIGGERS ? " triggers" : "", cal_stored.valid & GP_CAL_GYRO ? " gyro" : "");
}

/* 1 if saved. Failed: logged; the calibration in RAM is still the one used. */
static int cal_save(void)
{
    struct cal_record r = { GP_CAL_MAGIC, GP_CAL_VERSION, cal_stored };
    int n = gp_store_write(GP_CAL_ID, &r, sizeof(r));
    if (n != sizeof(r)) {
        gp_log("cal: save failed (%d of %d bytes): in use until power-off only", n, (int)sizeof(r));
        return 0;
    }
    return 1;
}

static void rest_restart(void)
{
    cal_n = 0;
    cal_restarts++;
}

static void band(u16 *lo, u16 *hi, u16 v)
{
    if (v < *lo) {
        *lo = v;
    }
    if (v > *hi) {
        *hi = v;
    }
}

void gp_cal_tick(void)
{
    if (cal_phase != GP_CAL_REST && cal_phase != GP_CAL_RANGE) {
        return;
    }
    struct gp_raw r;
    gp_board_raw(&r);
    cal_now = r;
    const s16 *g = gp_motion_gyro_raw;
    u8 m = cal_mask;
    if (cal_phase == GP_CAL_REST) {
        if ((m & GP_CAL_STICKS) && r.stick_ok != 0x0f) {
            return;                                      /* the stick chip hasn't sent every axis yet */
        }
        if (cal_n == 0) {
            cal_t0 = gp_ms();
            for (int i = 0; i < 4; i++) {
                st_sum[i] = 0;
                st_lo[i] = st_hi[i] = r.stick[i];
            }
            for (int i = 0; i < 2; i++) {
                tr_sum[i] = 0;
                tr_lo[i] = tr_hi[i] = r.trig[i];
            }
            for (int i = 0; i < 3; i++) {
                gy_sum[i] = 0;
                gy_lo[i] = gy_hi[i] = g[i];
            }
        }
        for (int i = 0; i < 4; i++) {
            band(&st_lo[i], &st_hi[i], r.stick[i]);
            st_sum[i] += r.stick[i];
            if ((m & GP_CAL_STICKS) && st_hi[i] - st_lo[i] > CAL_STICK_BAND) {
                rest_restart();                          /* a stick moved */
                return;
            }
        }
        for (int i = 0; i < 2; i++) {
            band(&tr_lo[i], &tr_hi[i], r.trig[i]);
            tr_sum[i] += r.trig[i];
            if ((m & GP_CAL_TRIGGERS) && tr_hi[i] - tr_lo[i] > CAL_TRIG_BAND) {
                rest_restart();
                return;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (g[i] < gy_lo[i]) {
                gy_lo[i] = g[i];
            }
            if (g[i] > gy_hi[i]) {
                gy_hi[i] = g[i];
            }
            gy_sum[i] += g[i];
            if ((m & GP_CAL_GYRO) && gy_hi[i] - gy_lo[i] > CAL_GYRO_BAND) {
                rest_restart();                          /* the pad moved */
                return;
            }
        }
        cal_n++;
        if (gp_ms() - cal_t0 < CAL_REST_MS) {
            return;
        }
        for (int i = 0; i < 4; i++) {
            st_centre[i] = st_min[i] = st_max[i] = st_sum[i] / cal_n;
        }
        for (int i = 0; i < 2; i++) {
            tr_rest[i] = tr_min[i] = tr_sum[i] / cal_n;
        }
        for (int i = 0; i < 3; i++) {
            gy_bias[i] = gy_sum[i] / cal_n;
        }
        cal_phase = (m & (GP_CAL_STICKS | GP_CAL_TRIGGERS)) ? GP_CAL_RANGE : GP_CAL_DONE;
        return;
    }
    /* RANGE: the furthest each way. The stick chip's odd bad frame is
     * filtered by hjc_stick (a jump needs a second frame). */
    for (int i = 0; i < 4; i++) {
        band(&st_min[i], &st_max[i], r.stick[i]);
    }
    for (int i = 0; i < 2; i++) {
        if (r.trig[i] < tr_min[i]) {
            tr_min[i] = r.trig[i];                       /* pressing lowers the value */
        }
    }
}

static void put16(u8 *p, u16 v)
{
    p[0] = v;
    p[1] = v >> 8;
}

/* How far the rest phase is (poll context; the tick moves cal_t0). */
static u8 cal_rest_pct(void)
{
    if (!cal_n) {
        return 0;
    }
    u32 ms = gp_ms() - cal_t0;
    return ms >= CAL_REST_MS ? 99 : ms * 100 / CAL_REST_MS;
}

/* Which parts of the mask fall short (GP_CAL_* bits). */
static u8 cal_short(void)
{
    u8 bad = 0;
    for (int i = 0; i < 4; i++) {
        if (st_max[i] - st_centre[i] < GP_CAL_STICK_MIN || st_centre[i] - st_min[i] < GP_CAL_STICK_MIN) {
            bad |= GP_CAL_STICKS;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (tr_rest[i] - tr_min[i] < GP_CAL_TRIG_MIN) {
            bad |= GP_CAL_TRIGGERS;
        }
    }
    return bad & cal_mask;
}

u8 gp_cal_op(u8 op, const u8 *a, int alen, u8 *out)
{
    switch (op) {
    case GP_CFG_OP_CAL_START: {
        u8 m = alen >= 1 ? a[0] & GP_CAL_ALL : 0;
        if (!m) {
            return GP_CFG_ERR_VALUE;
        }
        cal_phase = GP_CAL_IDLE;                         /* the tick stops sampling while this is set up */
        cal_mask = m;
        cal_n = 0;
        cal_restarts = 0;
        cal_phase = GP_CAL_REST;
        gp_log("cal: start (mask %x): rest", m);
        out[0] = m;
        return GP_CFG_OK;
    }
    case GP_CFG_OP_CAL_STATUS: {
        out[0] = cal_phase;
        out[1] = cal_mask;
        out[2] = cal_stored.valid;
        out[3] = cal_phase == GP_CAL_REST ? cal_rest_pct() : cal_phase > GP_CAL_REST ? 100 : 0;
        out[4] = cal_restarts > 255 ? 255 : cal_restarts;
        out[5] = cal_phase == GP_CAL_RANGE || cal_phase == GP_CAL_DONE ? cal_short() : 0;
        u8 *p = out + 6;                                 /* sticks: now, min, centre, max */
        for (int i = 0; i < 4; i++, p += 8) {
            put16(p, cal_now.stick[i]);
            put16(p + 2, st_min[i]);
            put16(p + 4, st_centre[i]);
            put16(p + 6, st_max[i]);
        }
        for (int i = 0; i < 2; i++, p += 6) {            /* triggers: now, rest, pressed */
            put16(p, cal_now.trig[i]);
            put16(p + 2, tr_rest[i]);
            put16(p + 4, tr_min[i]);
        }
        for (int i = 0; i < 3; i++, p += 2) {            /* gyro bias found */
            put16(p, gy_bias[i]);
        }
        return GP_CFG_OK;                                /* 6 + 32 + 12 + 6 = 56 bytes */
    }
    case GP_CFG_OP_CAL_FINISH: {
        if (cal_phase != GP_CAL_RANGE && cal_phase != GP_CAL_DONE) {
            return GP_CFG_ERR_STATE;                     /* not started, or still resting */
        }
        u8 bad = cal_short();
        out[0] = bad;
        if (bad) {
            return GP_CFG_ERR_VALUE;                     /* keep going: move further, FINISH again */
        }
        u8 m = cal_mask;
        cal_phase = GP_CAL_IDLE;
        if (m & GP_CAL_STICKS) {
            for (int i = 0; i < 4; i++) {
                cal_stored.stick_centre[i] = st_centre[i];
                cal_stored.stick_pos[i] = st_max[i] - st_centre[i];
                cal_stored.stick_neg[i] = st_centre[i] - st_min[i];
            }
        }
        if (m & GP_CAL_TRIGGERS) {
            for (int i = 0; i < 2; i++) {
                cal_stored.trig_rest[i] = tr_rest[i];
                cal_stored.trig_span[i] = tr_rest[i] - tr_min[i];
            }
        }
        if (m & GP_CAL_GYRO) {
            memcpy(cal_stored.gyro_bias, gy_bias, sizeof(gy_bias));
            gp_motion_set_bias(gy_bias);
        }
        cal_stored.valid |= m;
        int saved = cal_save();
        gp_board_cal_apply(&cal_stored, 0);
        out[1] = cal_stored.valid;
        if (!saved) {
            return GP_CFG_ERR_STORE;
        }
        gp_log("cal: stored (mask %x)", m);
        return GP_CFG_OK;
    }
    case GP_CFG_OP_CAL_CANCEL:
        cal_phase = GP_CAL_IDLE;
        return GP_CFG_OK;
    case GP_CFG_OP_CAL_CLEAR: {
        u8 m = alen >= 1 ? a[0] & GP_CAL_ALL : GP_CAL_ALL;
        cal_stored.valid &= ~m;
        int saved = cal_save();
        gp_board_cal_apply(&cal_stored, m);              /* cleared parts learn again (the gyro already does) */
        out[0] = cal_stored.valid;
        if (!saved) {
            return GP_CFG_ERR_STORE;
        }
        gp_log("cal: cleared (mask %x)", m);
        return GP_CFG_OK;
    }
    case GP_CFG_OP_CAL_GET: {
        out[0] = cal_stored.valid;
        u8 *p = out + 1;
        for (int i = 0; i < 4; i++, p += 6) {
            put16(p, cal_stored.stick_centre[i]);
            put16(p + 2, cal_stored.stick_pos[i]);
            put16(p + 4, cal_stored.stick_neg[i]);
        }
        for (int i = 0; i < 2; i++, p += 4) {
            put16(p, cal_stored.trig_rest[i]);
            put16(p + 2, cal_stored.trig_span[i]);
        }
        for (int i = 0; i < 3; i++, p += 2) {
            put16(p, cal_stored.gyro_bias[i]);
        }
        return GP_CFG_OK;                                /* 1 + 24 + 8 + 6 = 39 bytes */
    }
    default:
        return GP_CFG_ERR_OP;
    }
}
