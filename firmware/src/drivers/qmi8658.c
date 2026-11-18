/* qmi8658.c: see qmi8658.h.
 *
 * Set-up, as stock (C2 0x1E3784E..0x1E379DA), in order:
 *   0x60 = 0xB0   soft reset
 *   CTRL1 0x02 = 0x60   address auto-increment (bit 5 "BE" set, yet the data
 *                       comes little-endian: tested, gravity reads 0x2041 as 41 20)
 *   CTRL2 0x03 = 0x14   accel +-4 g (8192 LSB/g), ODR 448 Hz
 *   CTRL3 0x04 = 0x74   gyro +-2048 dps (16 LSB/dps), ODR 448 Hz
 *   CTRL5 0x06 = 0x55   accel and gyro low-pass on
 *   CAL1_L 0x0B = 0x01, CTRL9 0x0A = 0x12, CTRL9 = 0x00   (host command 0x12, then ack)
 *   CTRL7 0x08 = 0x83   accel + gyro on, sync-sample mode
 * Reading (C2 0x1E42E40): STATUSINT 0x2D until bits 0 and 1 are set (data
 * locked; here bit 0, new data, is enough), then 12 bytes from 0x35:
 * AX AY AZ GX GY GZ, little-endian. */
#include "system/includes.h"
#include "gamepad.h"
#include "jl_iic.h"
#include "qmi8658.h"

static const u8 qmi_init_seq[][2] = {
    { 0x02, 0x60 }, { 0x03, 0x14 }, { 0x04, 0x74 }, { 0x06, 0x55 },
    { 0x0B, 0x01 }, { 0x0A, 0x12 }, { 0x0A, 0x00 }, { 0x08, 0x83 },
};
static u8 qmi_addr;
static u8 qmi_state;                    /* 0 off, 1 reset sent, 2 running, 3 not found */
static u32 qmi_reset_t0;                /* the soft reset was sent */
static u8 qmi_whoami;
u32 qmi8658_ff, qmi8658_glitches;
static u32 qmi_jumps_real;

/* Raw units: accel 8192 / g (+-4 g), gyro 16 / dps (+-2048 dps). Between two
 * samples, 6000 is ~0.7 g and 8000 is 500 dps: a hard knock, not play. */
#define QMI_JUMP_ACC        6000
#define QMI_JUMP_GYR        8000

void qmi8658_init(u8 addr)
{
    qmi_addr = addr;
    if (jl_iic_init()) {
        qmi_state = 3;
        return;
    }
    jl_iic_write(qmi_addr, 0x60, 0xB0);                  /* soft reset; configure after it */
    qmi_state = 1;
    qmi_reset_t0 = gp_ms();
}

static void qmi_decode(const u8 *d, s16 *r)
{
    for (int i = 0; i < 6; i++) {
        r[i] = (s16)(d[2 * i] | (d[2 * i + 1] << 8));
    }
}

static int all_ff(const u8 *p, int n)
{
    while (n--) {
        if (*p++ != 0xff) {
            return 0;
        }
    }
    return 1;
}

static int qmi_jumps(const s16 *a, const s16 *b)
{
    for (int i = 0; i < 6; i++) {
        int d = a[i] - b[i];
        if (d < 0) {
            d = -d;
        }
        if (d > (i < 3 ? QMI_JUMP_ACC : QMI_JUMP_GYR)) {
            return 1;
        }
    }
    return 0;
}

int qmi8658_poll(s16 r[6])
{
    if (qmi_state == 1) {
        if (gp_ms() - qmi_reset_t0 < 50) {
            return 0;                                    /* the reset takes its time */
        }
        jl_iic_read(qmi_addr, 0x00, &qmi_whoami, 1);
        int ok = qmi_whoami == 0x05;
        for (int i = 0; ok && i < ARRAY_SIZE(qmi_init_seq); i++) {
            ok = jl_iic_write(qmi_addr, qmi_init_seq[i][0], qmi_init_seq[i][1]);
        }
        qmi_state = ok ? 2 : 3;
        return 0;
    }
    if (qmi_state != 2) {
        return 0;
    }
    u8 st, d[12];
    if (!jl_iic_read(qmi_addr, 0x2D, &st, 1)) {
        return 0;
    }
    if (!(st & 1) || !jl_iic_read(qmi_addr, 0x35, d, 12)) {
        return 0;                                        /* no new sample: keep the last one */
    }
    /* Seen (2026-11-13, debug log): now and then the 12 bytes come back all
     * 0xFF, the bus left idle: the sensor didn't drive SDA though the
     * address was acked. Never a real sample (six times -1): read again. */
    if (all_ff(d, 12)) {
        qmi8658_ff++;
        if (!jl_iic_read(qmi_addr, 0x35, d, 12) || all_ff(d, 12)) {
            return 0;                                    /* keep the last sample */
        }
    }
    qmi_decode(d, r);
    /* A sample that jumps implausibly from the last one: read again at once.
     * Seen once (2026-11-12): all six values wild for one report between
     * normal ones. If the second read agrees with the last sample, the first
     * was corrupt; if it agrees with the jump, it's real motion. */
    static s16 prev[6];
    static u8 have_prev;
    if (have_prev && qmi_jumps(r, prev)) {
        u8 d2[12] = {0};
        s16 r2[6];
        int ok2 = jl_iic_read(qmi_addr, 0x35, d2, 12);
        if (ok2) {
            qmi_decode(d2, r2);
        }
        if (ok2 && !qmi_jumps(r2, prev)) {
            qmi8658_glitches++;
            memcpy(r, r2, sizeof(r2));
        } else {
            qmi_jumps_real++;
        }
    }
    memcpy(prev, r, sizeof(prev));
    have_prev = 1;
    return 1;
}
