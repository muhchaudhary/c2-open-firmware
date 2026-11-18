/*
 * gamepad_c2.c: the GameSir Cyclone 2 as a gamepad (the board interface in
 * core/gamepad.h): which pin is which button, and the drivers behind the
 * sticks, triggers, IMU, rumble, battery and LEDs, with the C2's own
 * orientation and scaling. Pins: c2_pins.h, c2_power.h.
 */
#include "system/includes.h"
#include "app_config.h"

#ifdef CONFIG_BOARD_C2

#include "asm/adc_api.h"
#include "asm/power/p33.h"
#include "asm/iic_hw.h"
#include "c2_pins.h"
#include "c2_power.h"
#include "gamepad.h"
#include "gp_motion.h"
#include "gp_log.h"
#include "hall_trigger.h"
#include "stick_axis.h"
#include "hjc_stick.h"
#include "qmi8658.h"
#include "erm_rumble.h"
#include "battery_adc.h"
#include "ws2812_spi.h"

extern u8 c2_boot_rst_src, c2_boot_wkup_pnd;    /* board_c2.c */

/* ---- Identity -------------------------------------------------------------
 * Rumble (c2_pins.h): motor A PA09 = ch4 / timer4 (big, low frequency:
 * SInput left), motor B PB00 = ch1 / timer1 (small, high frequency: SInput
 * right), tested; 25 kHz as stock (0x1E377A0). Duty table per level as
 * stock (0x1E68CF6..0x1E68E82). */
static const struct erm_motor c2_motors[2] = {
    { C2_MOTOR_A, pwm_ch4, pwm_timer4, {
        { 0, 0, 0, 0 },
        { 5000, 100, 2000, 7000 },
        { 6000, 100, 3000, 8000 },
        { 7000, 100, 4000, 8000 },
        { 8000, 150, 4000, 8000 } } },
    { C2_MOTOR_B, pwm_ch1, pwm_timer1, {
        { 0, 0, 0, 0 },
        { 4000,  50, 2000, 6000 },
        { 5000, 100, 3000, 8000 },
        { 6000, 100, 3000, 9000 },
        { 7000, 100, 3000, 10000 } } },
};

const struct gp_board_info gp_board_info = {
    .name = GP_PRODUCT_NAME,
    .sinput = {
        .features = { 0xff, 0x02 },     /* rumble, player LEDs, accel, gyro, sticks, triggers; RGB */
        .gamepad_type = 3,              /* SDL_GAMEPAD_TYPE_XBOXONE */
        .face_style = 0x20,             /* ABXY */
        .accel_g = 4,
        .gyro_dps = 2048,
        .button_mask = { 0xff, 0xcf, 0x0f, 0x00 },   /* ABXY + d-pad; L3 R3 LB RB + paddles; Start Back Guide Capture */
    },
    /* 8 slots: 0 Home, 1-4 players (top to bottom), 5 left grip, 6 right
     * grip, 7 the charging dock's LED (through the dock's contacts; stock's
     * charging animation lights it in the fade-out, frame offset +21) */
    .leds = { .count = 8, .home = 0, .player = { 1, 2, 3, 4 }, .dock = 7 },
    .motors = c2_motors,
};

/* ---- Buttons: active low, pull-ups (board_c2.c sets the pins up) ---------- */
static const struct {
    u8 pin, button;
} c2_buttons[] = {
    { C2_KEY_SOUTH, GP_BTN_SOUTH }, { C2_KEY_EAST, GP_BTN_EAST },
    { C2_KEY_WEST, GP_BTN_WEST }, { C2_KEY_NORTH, GP_BTN_NORTH },
    { C2_KEY_UP, GP_BTN_UP }, { C2_KEY_DOWN, GP_BTN_DOWN },
    { C2_KEY_LEFT, GP_BTN_LEFT }, { C2_KEY_RIGHT, GP_BTN_RIGHT },
    { C2_KEY_L3, GP_BTN_L3 }, { C2_KEY_R3, GP_BTN_R3 },
    { C2_KEY_L1, GP_BTN_L1 }, { C2_KEY_R1, GP_BTN_R1 },
    { C2_KEY_PADDLE_A, GP_BTN_PADDLE_L1 }, { C2_KEY_PADDLE_B, GP_BTN_PADDLE_R1 },
    { C2_KEY_OPTIONS, GP_BTN_START }, { C2_KEY_SHARE, GP_BTN_BACK },
    { C2_KEY_HOME, GP_BTN_GUIDE }, { C2_KEY_CAPTURE, GP_BTN_CAPTURE },
    { C2_KEY_M, GP_BTN_MODIFIER },
};

u32 gp_board_buttons(void)
{
    u32 b = 0;
    for (int i = 0; i < ARRAY_SIZE(c2_buttons); i++) {
        if (!gpio_read(c2_buttons[i].pin)) {
            b |= GP_BIT(c2_buttons[i].button);
        }
    }
    return b;
}

/* ---- Triggers: hall sensors on the ADC (LT PB03, RT PB10) plus end-stop
 * switches on the PR pins (stock 0x1E507B8 / 0x1E6ADA8: PR1 = LT, PR0 = RT,
 * active low, "fully pressed"). Measured travel ~470 counts (rest 483 / 490,
 * 11 at full, 2026-11-12). */
static struct hall_trigger c2_lt = { .name = "LT", .adc_ch = AD_CH_PB3, .span_min = 440 };
static struct hall_trigger c2_rt = { .name = "RT", .adc_ch = AD_CH_PB10, .span_min = 440 };
#define C2_LT_ENDSTOP_PR    1
#define C2_RT_ENDSTOP_PR    0

static void c2_triggers_init(void)
{
    hall_trigger_init(&c2_lt);
    hall_trigger_init(&c2_rt);
    for (int n = 0; n < 2; n++) {                        /* PR0, PR1: input, pull-up, digital in */
        P33_CON_SET(R3_PR_DIR, n, 1, 1);
        P33_CON_SET(R3_PR_PD, n, 1, 0);
        P33_CON_SET(R3_PR_PU, n, 1, 1);
        P33_CON_SET(R3_PR_DIE, n, 1, 1);
    }
}

/* ---- Sticks: the stick chip on UART RX PA03 (hjc_stick.h). Directions
 * (tested in SDL, 2026-11-12): the left stick as is, the right stick
 * inverted on both axes (0x7FFF ^ v). (Stock inverts LY and RX, in its own
 * convention.) Raw values: 15 bits, rest ~16384, swing ~10000-12500
 * (2026-11-12); stock's margin 753. The dead zone (stock: a stored
 * percentage) is a setting now: gp_tuning.h, default 4 %. */
static const struct stick_axis_cfg c2_stick_cfg = {
    .span0 = 10000, .margin = 753, .lo = 1000, .hi = 31767, .still = 300, .recentre = 400,
    .avg = 1,                           /* hjc_stick averages the chip's stream (docs/latency.md) */
};
/* Stock's way, for comparison (log command "stickfilter old"): the latest
 * value, then a mean of 6 reports. */
static const struct stick_axis_cfg c2_stick_cfg_old = {
    .span0 = 10000, .margin = 753, .lo = 1000, .hi = 31767, .still = 300, .recentre = 400,
    .avg = 6,
};
static volatile u8 c2_filter_req;                        /* 1 new, 2 old: from the log task */
static struct stick_axis c2_axes[4];

static void c2_sticks_init(void)
{
    static const char *const name[4] = { "LX", "LY", "RX", "RY" };
    for (int i = 0; i < 4; i++) {
        stick_axis_init(&c2_axes[i], name[i], &c2_stick_cfg);
    }
    hjc_stick_init(C2_STICK_RX);
    /* PA03 belongs to the hardware pin group PA02 (TX) / PA03 (RX), and the
     * driver takes PA02 too even without a TX pin: D-pad Up then read
     * pressed. Stock opens the UART and right after puts PA02 back as the
     * Up button (input_port_init at 0x1E3773C); same here. */
    gpio_set_direction(C2_KEY_UP, 1);
    gpio_set_pull_down(C2_KEY_UP, 0);
    gpio_set_pull_up(C2_KEY_UP, 1);
    gpio_set_die(C2_KEY_UP, 1);
    /* PA08: the stick chip's power, on when high (tested; stock calls it the
     * "audio power amplifier"). */
    gpio_direction_output(C2_PA08_OUT, 1);
}


/* ---- Stick latency probe (log command "stickprobe", docs/latency.md).
 * "stickprobe": 3 s hands off, then the noise per axis of the value the
 * stick scaling gets (after the average, before the dead zone: reports read
 * 0 at rest). Then flick the sticks: each time a raw value from the stick
 * chip passes C2_PROBE_THRESH from the centre, the time from that frame's
 * arrival (hjc_stick's probe) until the averaged value in the report being
 * built passes it too: the stick's own path inside the pad (UART burst,
 * waiting for the tick, the average), not the link. "stickprobe end": the
 * summary. */
#define C2_PROBE_THRESH     4000                         /* raw: ~40 % of the travel */
#define C2_PROBE_QUIET_MS   3000
#define C2_PROBE_MISS_US    200000                       /* the average never got there: a tap too short */
#define C2_PROBE_MAX        128
enum { C2_PROBE_OFF, C2_PROBE_QUIET, C2_PROBE_FLICK };
static volatile u8 c2_probe_start, c2_probe_end;         /* from the log task */
static volatile u8 c2_probe_noise_due, c2_probe_sum_due;  /* for the poll to log */
static u8 c2_probe;
static u32 c2_probe_t0;
static struct {
    u32 n;
    s32 sum;
    u32 sumsq;
    s16 lo, hi;
} c2_noise[4];
static struct {
    u8 pending;
    s8 dir;
    u32 t_raw;
} c2_pf[4];
static u16 c2_lag[C2_PROBE_MAX];                         /* 10 us units */
static u16 c2_lag_n, c2_lag_missed;

static u16 c2_chip_centre(int i)
{
    u16 c = c2_axes[i].centre;
    return i >= HJC_RX ? 0x7fff ^ c : c;                 /* the probe compares in the chip's direction */
}

static void c2_probe_tick(void)
{
    if (c2_filter_req) {
        int old = c2_filter_req == 2;
        c2_filter_req = 0;
        hjc_stick_mean(!old);
        for (int i = 0; i < 4; i++) {
            c2_axes[i].cfg = old ? &c2_stick_cfg_old : &c2_stick_cfg;
            c2_axes[i].hist_n = 0;
        }
        gp_log("sticks: filter %s", old ? "old (latest value, mean of 6 reports)" : "new (mean of the stream since the last report)");
    }
    if (c2_probe_start) {
        c2_probe_start = 0;
        hjc_probe_off();
        memset(c2_noise, 0, sizeof(c2_noise));
        memset(c2_pf, 0, sizeof(c2_pf));
        c2_lag_n = c2_lag_missed = 0;
        c2_probe = C2_PROBE_QUIET;
        c2_probe_t0 = gp_ms();
        gp_log("probe: hands off the sticks for %u s", C2_PROBE_QUIET_MS / 1000);
    }
    if (c2_probe_end) {
        c2_probe_end = 0;
        hjc_probe_off();
        if (c2_probe != C2_PROBE_OFF) {
            c2_probe = C2_PROBE_OFF;
            c2_probe_sum_due = 1;
        }
    }
    if (c2_probe == C2_PROBE_QUIET && gp_ms() - c2_probe_t0 >= C2_PROBE_QUIET_MS) {
        c2_probe = C2_PROBE_FLICK;
        c2_probe_noise_due = 1;
        for (int i = 0; i < 4; i++) {
            hjc_probe_arm(i, c2_chip_centre(i), C2_PROBE_THRESH);
        }
    }
}

static void c2_probe_axis(int i)
{
    const struct stick_axis *a = &c2_axes[i];
    if (c2_probe == C2_PROBE_OFF || a->phase != STICK_READY) {
        return;
    }
    s32 d = (s32)a->filt - a->centre;
    if (c2_probe == C2_PROBE_QUIET) {
        typeof(c2_noise[0]) *z = &c2_noise[i];
        if (d < -2000 || d > 2000) {
            return;                                      /* touched: not noise */
        }
        if (!z->n || d < z->lo) {
            z->lo = d;
        }
        if (!z->n || d > z->hi) {
            z->hi = d;
        }
        z->n++;
        z->sum += d;
        z->sumsq += d * d;
        return;
    }
    u32 t;
    int dir;
    if (hjc_probe_take(i, &t, &dir)) {
        c2_pf[i].pending = 1;
        c2_pf[i].dir = i >= HJC_RX ? -dir : dir;         /* the chip's direction -> ours (RX, RY inverted) */
        c2_pf[i].t_raw = t;
    }
    if (!c2_pf[i].pending) {
        return;
    }
    u32 span = gp_us_span(c2_pf[i].t_raw, gp_us());
    if (d * c2_pf[i].dir >= C2_PROBE_THRESH) {          /* on the raw value's side (not a lagging other one) */
        c2_pf[i].pending = 0;
        gp_log("probe: %s %u.%u ms", c2_axes[i].name, (unsigned)(span / 1000), (unsigned)(span % 1000 / 100));
        if (c2_lag_n < C2_PROBE_MAX) {
            c2_lag[c2_lag_n++] = span / 10 > 0xffff ? 0xffff : span / 10;
        }
    } else if (span > C2_PROBE_MISS_US) {
        c2_pf[i].pending = 0;
        c2_lag_missed++;
    }
}

static u32 c2_isqrt(u32 n)
{
    u32 r = 0;
    while ((r + 1) * (r + 1) <= n) {
        r++;
    }
    return r;
}

/* Poll context: the logs (sorting and printing stay out of the tick). */
static void c2_probe_poll(void)
{
    if (c2_probe_noise_due) {
        c2_probe_noise_due = 0;
        for (int i = 0; i < 4; i++) {
            const typeof(c2_noise[0]) *z = &c2_noise[i];
            if (!z->n) {
                gp_log("probe: noise %s: no readings", c2_axes[i].name);
                continue;
            }
            s32 mean = z->sum / (s32)z->n;
            s32 var = (s32)(z->sumsq / z->n) - mean * mean;
            gp_log("probe: noise %s: %u reads, sd %u, p-p %d (raw, after the average)",
                   c2_axes[i].name, (unsigned)z->n, (unsigned)c2_isqrt(var > 0 ? var : 0), z->hi - z->lo);
        }
        gp_log("probe: now flick the sticks past ~40 %%, back to rest each time; \"stickprobe end\" for the summary");
    }
    if (c2_probe_sum_due) {
        c2_probe_sum_due = 0;
        u16 n = c2_lag_n;
        for (int i = 1; i < n; i++) {                    /* insertion sort, at most C2_PROBE_MAX */
            u16 v = c2_lag[i];
            int j = i;
            for (; j > 0 && c2_lag[j - 1] > v; j--) {
                c2_lag[j] = c2_lag[j - 1];
            }
            c2_lag[j] = v;
        }
        if (!n) {
            gp_log("probe: no flicks (%u missed)", c2_lag_missed);
            return;
        }
        u32 sum = 0;
        for (int i = 0; i < n; i++) {
            sum += c2_lag[i];
        }
#define MS(x) (unsigned)((x) / 100), (unsigned)((x) % 100 / 10)
        gp_log("probe: %u flicks (%u missed): median %u.%u ms, mean %u.%u, min %u.%u, max %u.%u",
               n, c2_lag_missed, MS(c2_lag[n / 2]), MS(sum / n), MS(c2_lag[0]), MS(c2_lag[n - 1]));
#undef MS
    }
}

static u16 c2_stick_raw[4];
static u8 c2_stick_ok;

static int c2_axis(int i)
{
    u16 v;
    if (!hjc_stick_value(i, &v)) {
        return 0;
    }
    if (i >= HJC_RX) {
        v = 0x7fff ^ v;                                  /* RX, RY (tested in SDL) */
    }
    c2_stick_raw[i] = v;                                 /* for calibration: what stick_axis sees */
    c2_stick_ok |= BIT(i);
    if (gpio_read(i >= HJC_RX ? C2_KEY_R3 : C2_KEY_L3) == 0) {   /* clicked (active low) */
        stick_axis_hold(&c2_axes[i]);
    }
    int out = stick_axis_read(&c2_axes[i], v);
    c2_probe_axis(i);
    return out;
}

/* ---- IMU: a QMI8658 at 0xD4 on hardware IIC group C (SCL PB04, SDA PB06),
 * 400 kHz, HD on, pulls off. Stock's DS4 frame is (AY, AZ, AX) / (GY, GZ,
 * GX), which is SDL's frame; SDL's SInput driver maps SInput (X, Y, Z) to
 * SDL (-X, Z, -Y), so SInput X = -AY, Y = -AX, Z = AZ, the same for the
 * gyro. The ranges are SInput's own (features reply: 4 g, 2048 dps), so no
 * scaling. */
const struct hw_iic_config hw_iic_cfg[] = {
    { .port = 'C', .baudrate = 400000, .hdrive = 1, .io_filter = 1, .io_pu = 0, .role = IIC_MASTER },
};
#define C2_IMU_ADDR         0xD4

/* ---- IIC bus scan (docs/stock-features.md): which devices answer on the
 * IMU's bus. Stock has code for a 24C64 EEPROM (0xA0..0xAE) and a PlayStation
 * auth chip, maybe for other boards only. Each address gets an address-only
 * write (jl_iic_probe: no data byte, nothing is written). It runs in the
 * tick, between IMU reads (the only other user of the bus), 8 addresses per
 * tick. The tick runs only while a host is connected, so the scan does too:
 * once 2 s into the first connection, and again on the log command "i2c"
 * (kept until a host connects). The 7-bit
 * range 0x08..0x77, as i2cdetect; the log names them in 8-bit form, as the
 * IMU's 0xD4. */
#include "jl_iic.h"
#include <string.h>

#define C2_SCAN_FIRST       0x08
#define C2_SCAN_LAST        0x77
#define C2_SCAN_PER_TICK    8
static volatile u8 c2_scan_req;
static u8 c2_scan_ready;                                 /* the bus is set up (gp_board_start; not in charging mode) */
static u8 c2_scan_boot;                                  /* the first scan: 1 to come, 2 connected since ... */
static u32 c2_scan_boot_t0;                              /* ... this (the first tick), 0 done */
static u8 c2_scan_addr;                                  /* 7-bit; 0 = not scanning */
static u8 c2_scan_found[16], c2_scan_n;

static void c2_scan_tick(void)
{
    if (c2_scan_boot == 1) {
        c2_scan_boot = 2;
        c2_scan_boot_t0 = gp_ms();
    } else if (c2_scan_boot == 2 && gp_ms() - c2_scan_boot_t0 >= 2000) {
        c2_scan_boot = 0;
        c2_scan_req = 1;
    }
    if (!c2_scan_addr) {
        if (!c2_scan_req || !c2_scan_ready) {
            return;
        }
        c2_scan_req = 0;
        c2_scan_addr = C2_SCAN_FIRST;
        c2_scan_n = 0;
    }
    for (int k = 0; k < C2_SCAN_PER_TICK && c2_scan_addr <= C2_SCAN_LAST; k++, c2_scan_addr++) {
        if (jl_iic_probe(c2_scan_addr << 1) && c2_scan_n < sizeof(c2_scan_found)) {
            c2_scan_found[c2_scan_n++] = c2_scan_addr << 1;
        }
    }
    if (c2_scan_addr <= C2_SCAN_LAST) {
        return;
    }
    c2_scan_addr = 0;
    char line[16 * 5 + 1];
    int p = 0;
    for (int i = 0; i < c2_scan_n; i++) {
        p += sprintf(line + p, " %02X", c2_scan_found[i]);
    }
    line[p] = 0;
    gp_log("i2c: scan %02X..%02X (8-bit addresses): %u found:%s (IMU is %02X)",
           C2_SCAN_FIRST << 1, C2_SCAN_LAST << 1, c2_scan_n, c2_scan_n ? line : " none", C2_IMU_ADDR);
}

/* ---- EEPROM dump (read-only): the device at 0xA0 found by the scan,
 * stock's 24C64 (8 KB, 2-byte addresses; docs/stock-features.md). Log
 * command "eeprom" dumps all of it, "eeprom AAAA NNNN" (hex) a range. One
 * 32-byte line every C2_EE_EVERY ticks (~30 ms), well under what the log
 * port carries (128 B per 20 ms), so no line is dropped:
 *   ee: 01E0: <64 hex digits>
 *   ee: done 0000+2000 sum 1A2B3C failed 0
 * (sum: the bytes added up, for tools/c2eeprom.py to check). Like the scan,
 * it runs in the tick, so only while a host is connected. Nothing is ever
 * written to the EEPROM: a read sets the address pointer only. */
#define C2_EE_ADDR          0xA0
#define C2_EE_SIZE          0x2000
#define C2_EE_LINE          32
#define C2_EE_EVERY         8
static volatile u16 c2_ee_req_from, c2_ee_req_len;      /* a request from the log command */
static volatile u8 c2_ee_req;
static u16 c2_ee_at, c2_ee_end, c2_ee_from;
static u8 c2_ee_busy, c2_ee_wait;
static u32 c2_ee_sum, c2_ee_failed;

static void c2_ee_tick(void)
{
    if (!c2_ee_busy) {
        if (!c2_ee_req || !c2_scan_ready || c2_scan_addr) {
            return;
        }
        c2_ee_req = 0;
        c2_ee_from = c2_ee_at = c2_ee_req_from;
        c2_ee_end = c2_ee_req_from + c2_ee_req_len;
        c2_ee_sum = c2_ee_failed = 0;
        c2_ee_wait = 0;
        c2_ee_busy = 1;
    }
    if (c2_ee_wait && --c2_ee_wait) {
        return;
    }
    c2_ee_wait = C2_EE_EVERY;
    u16 n = c2_ee_end - c2_ee_at < C2_EE_LINE ? c2_ee_end - c2_ee_at : C2_EE_LINE;
    u8 d[C2_EE_LINE];
    if (jl_iic_read16(C2_EE_ADDR, c2_ee_at, d, n)) {
        char hex[C2_EE_LINE * 2 + 1];
        for (int i = 0; i < n; i++) {
            sprintf(hex + 2 * i, "%02X", d[i]);
            c2_ee_sum += d[i];
        }
        hex[2 * n] = 0;
        gp_log("ee: %04X: %s", c2_ee_at, hex);
    } else {
        c2_ee_failed++;
        gp_log("ee: %04X: read failed", c2_ee_at);
    }
    c2_ee_at += n;
    if (c2_ee_at >= c2_ee_end) {
        c2_ee_busy = 0;
        gp_log("ee: done %04X+%04X sum %06X failed %u", c2_ee_from, c2_ee_end - c2_ee_from,
               (unsigned)c2_ee_sum, (unsigned)c2_ee_failed);
    }
}

static u32 c2_hex(const char **p)
{
    u32 v = 0;
    while (**p == ' ') {
        (*p)++;
    }
    for (; **p; (*p)++) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            break;
        }
        v = v * 16 + d;
    }
    return v;
}

/* ---- EEPROM writes (drivers/eeprom24.c), task context. Log command
 * "eetest": write a 40-byte pattern across the page boundary at 0x1FE0
 * (0x1FC8..0x1FEF, the last two pages, unused: 0xFF in the stock layout),
 * read it back, then put the old bytes back and check again. */
#include "eeprom24.h"

const struct eeprom24 c2_eeprom = { .addr = C2_EE_ADDR, .size = C2_EE_SIZE, .page = 32 };

static void c2_ee_selftest(void)
{
    u8 old[64], pat[40], back[64];
    const u16 base = 0x1FC0, at = 0x1FC8;
    u32 t0 = jiffies_msec();
    int r = eeprom24_read(&c2_eeprom, base, old, sizeof(old));
    if (r) {
        gp_log("eetest: read %04X failed (%d)", base, r);
        return;
    }
    for (int i = 0; i < sizeof(pat); i++) {
        pat[i] = 0x5A ^ (i * 37) ^ (u8)t0;               /* differs per run, so the pages really get written */
    }
    u32 w0 = eeprom24_pages_written, s0 = eeprom24_pages_skipped;
    u32 t1 = jiffies_msec();
    r = eeprom24_write(&c2_eeprom, at, pat, sizeof(pat));
    u32 t2 = jiffies_msec();
    int rb = r ? r : eeprom24_read(&c2_eeprom, at, back, sizeof(pat));
    int same = !rb && !memcmp(back, pat, sizeof(pat));
    gp_log("eetest: write 40 B at %04X: %d (%u pages written, %u skipped, %u ms), read back %s",
           at, r, (unsigned)(eeprom24_pages_written - w0), (unsigned)(eeprom24_pages_skipped - s0),
           (unsigned)(t2 - t1), same ? "matches" : "DIFFERS");
    /* the same bytes again: nothing to write */
    w0 = eeprom24_pages_written;
    r = eeprom24_write(&c2_eeprom, at, pat, sizeof(pat));
    gp_log("eetest: same bytes again: %d, %u pages written (expect 0)", r,
           (unsigned)(eeprom24_pages_written - w0));
    /* restore */
    r = eeprom24_write(&c2_eeprom, base, old, sizeof(old));
    rb = r ? r : eeprom24_read(&c2_eeprom, base, back, sizeof(old));
    same = !rb && !memcmp(back, old, sizeof(old));
    gp_log("eetest: restore %04X+%u: %d, %s; total %u ms", base, (unsigned)sizeof(old), r,
           same ? "back as before" : "NOT as before", (unsigned)(jiffies_msec() - t0));
}

/* Log command "factory": stock's factory calibration as copied into our
 * EEPROM layout (c2_store.c), decoded (docs/stock-features.md: 32 big-endian
 * u16 for sticks and triggers, then 6 little-endian s16 IMU offsets). */
int c2_store_factory(u8 *buf, int len);

static void c2_factory_log(void)
{
    u8 f[1 + 66 + 14];
    if (c2_store_factory(f, sizeof(f)) != sizeof(f)) {
        gp_log("factory: no factory record (EEPROM not in use?)");
        return;
    }
    if (f[0] & 1) {
        const u8 *c = f + 1;
#define BE(i) (c[2 * (i)] << 8 | c[2 * (i) + 1])
        gp_log("factory: L centre %u,%u x %u..%u y %u..%u | R centre %u,%u x %u..%u y %u..%u",
               BE(0), BE(1), BE(4), BE(5), BE(6), BE(7), BE(2), BE(3), BE(8), BE(9), BE(10), BE(11));
        gp_log("factory: triggers LT %u..%u RT %u..%u", BE(30), BE(28), BE(31), BE(29));
#undef BE
    } else {
        gp_log("factory: no stick/trigger calibration");
    }
    if (f[0] & 2) {
        const u8 *m = f + 1 + 66;
#define LE(i) (s16)(m[2 * (i)] | m[2 * (i) + 1] << 8)
        gp_log("factory: imu accel %d %d %d gyro %d %d %d", LE(0), LE(1), LE(2), LE(3), LE(4), LE(5));
#undef LE
    } else {
        gp_log("factory: no imu offsets");
    }
}

enum { C2_CMD_NONE, C2_CMD_FACTORY, C2_CMD_EETEST };
static volatile u8 c2_cmd_pending;                       /* set by the log task, run by gp_board_poll */

static void c2_cmd_run(void)
{
    u8 c = c2_cmd_pending;
    c2_cmd_pending = C2_CMD_NONE;
    if (c == C2_CMD_FACTORY) {
        c2_factory_log();
    } else if (c == C2_CMD_EETEST) {
        if (!c2_scan_ready) {
            gp_log("cmd: eetest: the bus is off (charging mode)");
        } else if (c2_ee_busy || c2_scan_addr) {
            gp_log("cmd: eetest: a dump or scan is running, try again after it");
        } else {
            gp_log("cmd: eetest");
            c2_ee_selftest();
        }
    }
}

int gp_log_board_command(const char *cmd)
{
    /* Commands that do real work run in the app task (gp_board_poll), not
     * here in the log task: its stack is 1 KB, and "factory" (a buffer plus
     * a 13-argument gp_log) overflowed it and reset the pad (2026-11-17);
     * an EEPROM write would also stall the log while it waits. */
    if (!strcmp(cmd, "factory")) {
        c2_cmd_pending = C2_CMD_FACTORY;
        return 1;
    }
    if (!strcmp(cmd, "eetest")) {
        c2_cmd_pending = C2_CMD_EETEST;
        return 1;
    }
    if (!strcmp(cmd, "stickprobe")) {
        c2_probe_start = 1;
        return 1;
    }
    if (!strcmp(cmd, "stickprobe end")) {
        c2_probe_end = 1;
        return 1;
    }
    if (!strcmp(cmd, "stickfilter new") || !strcmp(cmd, "stickfilter old")) {
        c2_filter_req = cmd[12] == 'o' ? 2 : 1;
        return 1;
    }
    if (!strcmp(cmd, "i2c")) {
        gp_log(c2_scan_ready ? "cmd: i2c scan" : "cmd: i2c: the bus is off (charging mode)");
        c2_scan_req = 1;
        return 1;
    }
    if (!strncmp(cmd, "eeprom", 6) && (cmd[6] == 0 || cmd[6] == ' ')) {
        const char *p = cmd + 6;
        u32 from = 0, len = C2_EE_SIZE;
        if (*p) {
            from = c2_hex(&p);
            len = c2_hex(&p);
        }
        if (from >= C2_EE_SIZE || !len) {
            gp_log("cmd: eeprom: range %X+%X is outside 0000..%04X (eeprom [AAAA NNNN], hex)",
                   (unsigned)from, (unsigned)len, C2_EE_SIZE - 1);
            return 1;
        }
        if (from + len > C2_EE_SIZE) {
            len = C2_EE_SIZE - from;
        }
        if (!c2_scan_ready) {
            gp_log("cmd: eeprom: the bus is off (charging mode)");
            return 1;
        }
        gp_log("cmd: eeprom dump %04X+%04X%s", (unsigned)from, (unsigned)len,
               c2_ee_busy ? " (after the one running)" : "");
        c2_ee_req_from = from;
        c2_ee_req_len = len;
        c2_ee_req = 1;
        return 1;
    }
    return 0;
}

/* ---- Battery (PB01 through a 1:2 divider, stock ADC slot 3, 0x1E35896).
 * Percent from tables measured on this pad (2026-11-15/16, tools/c2batlog.py,
 * firmware/build/batlog-*.csv): on battery, an idle discharge from the
 * charger's own "done" to the SDK's power-off (19 h 35 min; time stands for
 * charge, the idle draw being steady), 0 % = the power-off. On 5 V, a charge
 * from empty (1 h 53 min to the charger's "done"; the constant-voltage end,
 * from 4195 mV, counted at half the rate as its current tapers). Stock's
 * straight 3250..4100 mV read up to 16 % high below 30 % and still said 7 %
 * at the power-off; plugged in at empty it read 41 %. "Full" is the charger's
 * status pin (PB02: low while charging), stock's 4000 mV for 30 s fired ~65
 * min early. SDK low-battery warning / power-off at 3.4 / 3.3 V. The chip's
 * own VBAT is not the battery on the C2 (a flat 4548 mV). */
static const struct battery_point c2_on_battery[] = {
    { 4128, 100 }, { 4052, 95 }, { 4023, 90 }, { 3985, 85 }, { 3940, 80 },
    { 3904, 75 }, { 3872, 70 }, { 3831, 65 }, { 3781, 60 }, { 3727, 55 },
    { 3685, 50 }, { 3656, 45 }, { 3631, 40 }, { 3614, 35 }, { 3595, 30 },
    { 3573, 25 }, { 3549, 20 }, { 3516, 15 }, { 3474, 10 }, { 3432, 5 },
    { 3318, 0 },
};
static const struct battery_point c2_charging[] = {
    { 4203, 99 }, { 4185, 90 }, { 4167, 80 }, { 4149, 70 }, { 4113, 60 },
    { 4075, 50 }, { 3969, 40 }, { 3875, 30 }, { 3826, 20 }, { 3751, 10 },
    { 3604, 0 },
};

static int c2_charger_done(void)
{
    return gp_board_charger_pin() == 1;
}

static const struct battery_adc_cfg c2_battery = {
    .adc_ch = GP_BATTERY_ADC_CH, .divider = GP_BATTERY_DIVIDER,
    .on_battery = c2_on_battery, .on_battery_n = ARRAY_SIZE(c2_on_battery),
    .charging = c2_charging, .charging_n = ARRAY_SIZE(c2_charging),
    .charger_done = c2_charger_done,
    .warn_mv = 3400, .off_mv = 3300,
};

static u8 c2_battery_on;

static void c2_battery_init(void)
{
    if (!c2_battery_on) {                                /* also in charging mode, which skips gp_board_start */
        c2_battery_on = 1;
        battery_adc_init(&c2_battery);
    }
}

/* ---- The board interface -------------------------------------------------- */
void gp_board_start(void)
{
    gp_log("boot: rst_src %02x wkup_pnd %02x", c2_boot_rst_src, c2_boot_wkup_pnd);
    c2_triggers_init();
    c2_battery_init();
    qmi8658_init(C2_IMU_ADDR);
    c2_scan_ready = 1;
    c2_scan_boot = 1;
    c2_sticks_init();
}

void gp_board_tick(void)
{
    s16 r[6];
    if (jl_iic_task_busy) {
        return;                                          /* task code is mid-transfer (eeprom24.c): skip the bus */
    }
    c2_scan_tick();
    c2_ee_tick();
    if (qmi8658_poll(r)) {
        s16 acc[3] = { gp_neg(r[1]), gp_neg(r[0]), r[2] };
        s16 gyr[3] = { gp_neg(r[4]), gp_neg(r[3]), r[5] };
        gp_motion_update(acc, gyr);
    }
}

void gp_board_read(struct gp_state *s)
{
    /* Per axis, scaled; the dead zone and the rest of the shaping are the
     * core's (gp_tuning.h, a setting per profile). */
    c2_probe_tick();
    s->lx = c2_axis(HJC_LX);
    s->ly = c2_axis(HJC_LY);
    s->rx = c2_axis(HJC_RX);
    s->ry = c2_axis(HJC_RY);
    u8 pr_in = p33_rx_1byte(R3_PR_IN);
    s->lt = hall_trigger_read(&c2_lt, !(pr_in & BIT(C2_LT_ENDSTOP_PR)));
    s->rt = hall_trigger_read(&c2_rt, !(pr_in & BIT(C2_RT_ENDSTOP_PR)));
    if (hjc_stick_baud_check()) {
        for (int i = 0; i < 4; i++) {                    /* relearn the centres at the new rate */
            stick_axis_relearn(&c2_axes[i]);
        }
    }
    memcpy(s->imu, gp_motion, sizeof(s->imu));
}

void gp_board_raw(struct gp_raw *r)
{
    memcpy(r->stick, c2_stick_raw, sizeof(r->stick));
    r->stick_ok = c2_stick_ok;
    r->trig[0] = c2_lt.raw;
    r->trig[1] = c2_rt.raw;
}

void gp_board_cal_apply(const struct gp_cal *c, u8 clear)
{
    struct hall_trigger *t[2] = { &c2_lt, &c2_rt };
    OS_ENTER_CRITICAL();                                 /* the tick reads these */
    for (int i = 0; i < 4; i++) {
        if (c->valid & GP_CAL_STICKS) {
            stick_axis_set_cal(&c2_axes[i], c->stick_centre[i], c->stick_pos[i], c->stick_neg[i]);
        } else if (clear & GP_CAL_STICKS) {
            stick_axis_clear_cal(&c2_axes[i]);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (c->valid & GP_CAL_TRIGGERS) {
            hall_trigger_set_cal(t[i], c->trig_rest[i], c->trig_span[i]);
        } else if (clear & GP_CAL_TRIGGERS) {
            hall_trigger_clear_cal(t[i]);
        }
    }
    OS_EXIT_CRITICAL();
}

void gp_board_battery(struct gp_battery *b)
{
    c2_battery_init();
    battery_adc_update(gp_board_charge_power(), b);
}

int gp_board_usb_power(void)
{
    return gpio_read(C2_5V_DETECT);
}

/* ---- The charging dock ("pedestal", c2_pins.h). Stock (0x1E37B04 at boot,
 * 0x1E683E2 at runtime): PA01 in mV x 151 / 100, graded <= 1600 / <= 4000 /
 * above, a new grade taken after 20 equal readings. Above 4000 = the dock's
 * 5 V: PB05 on ("open pedestal power"; at boot directly, at runtime then a
 * soft reset, "pedestal 5v insert reset"). Lower: PB05 off ("close
 * pedestal power"; in charging mode then off, "pedestal change sleep"). */
#define C2_DOCK_ADC_CH      AD_CH_PA1
#define C2_DOCK_MV          4000
#define C2_DOCK_LOW_MV      1600
#define C2_DOCK_DEBOUNCE    20                           /* readings, GP_POLL_MS apart */
static u8 c2_dock_on, c2_dock, c2_dock_known, c2_dock_grade = 0xff, c2_dock_count;
static u16 c2_dock_mv;

static u32 c2_dock_read_mv(void)
{
    return adc_get_voltage(C2_DOCK_ADC_CH) * 151 / 100;
}

void gp_board_dock_poll(void)
{
    if (!c2_dock_on) {
        c2_dock_on = 1;
        adc_add_sample_ch(C2_DOCK_ADC_CH);
        return;                                          /* the first value comes with the next ADC round */
    }
    u32 mv = c2_dock_read_mv();
    c2_dock_mv = mv;
    u8 grade = mv > C2_DOCK_MV ? 2 : mv > C2_DOCK_LOW_MV ? 1 : 0;
    if (grade != c2_dock_grade) {
        c2_dock_grade = grade;
        c2_dock_count = 0;
        return;
    }
    if (c2_dock_count < C2_DOCK_DEBOUNCE) {
        if (++c2_dock_count == C2_DOCK_DEBOUNCE) {
            u8 docked = grade == 2;
            c2_dock_known = 1;
            gp_log("dock: %u mV, grade %u%s", (unsigned)mv, grade, docked ? ", dock power on" : "");
            if (docked != c2_dock) {
                c2_dock = docked;
                gpio_direction_output(C2_PEDESTAL_PWR, docked);
            }
        }
    }
}

int gp_board_docked(void)
{
    return c2_dock_known ? c2_dock : -1;
}

int gp_board_charge_power(void)
{
    return gp_board_usb_power() || c2_dock;
}

/* PB02: the charger IC's status output (stock: input with pull-up, never
 * read). Low while the charger works on 5 V; high once it is done, and
 * with no 5 V. Seen 2026-11-15/16: high at the moment the voltage left
 * ~4.2 V, twice (a top-off and a charge from empty). */
int gp_board_charger_pin(void)
{
    return gpio_read(IO_PORTB_02);
}

/* "Extend power" (PB09, on when low) feeds the whole LED chain. */
void gp_board_led_power(int on)
{
    gpio_direction_output(C2_EXT_POWER, on ? 0 : 1);
}

void gp_board_led_send(const u8 (*rgb)[3], int n)
{
    ws2812_spi_send(rgb, n);
}

void gp_board_power_down(void)
{
    gp_board_led_power(0);
    gpio_direction_output(C2_PA08_OUT, 0);               /* stick chip power off */
}

/* Woken from off by USB 5 V alone (wake port 2, PC03; Home is port 1).
 * P3_WKUP_PND reads 0xFF after a flash / cold start, so those boot normally. */
#define C2_WKUP_HOME        BIT(1)
#define C2_WKUP_5V          BIT(2)

int gp_board_woken_by_usb_only(void)
{
    u8 w = c2_boot_wkup_pnd;
    return w != 0xff && (w & C2_WKUP_5V) && !(w & C2_WKUP_HOME) && gpio_read(C2_KEY_HOME);
}

void gp_board_poll(void)
{
    hjc_stick_clock_check();
    c2_probe_poll();
    if (c2_cmd_pending) {
        c2_cmd_run();
    }
}

int gp_board_status(char *buf, int len)
{
    static u32 frames, bad, imu_ff;
    u32 ok = hjc_stick_frames - frames, nbad = hjc_stick_bad - bad;
    int n = snprintf(buf, len, "stick errors %u%% | imu re-reads %u | bat %u mV%s pb02 %d | dock %u mV%s",
                     (unsigned)(ok + nbad ? nbad * 100 / (ok + nbad) : 0), (unsigned)(qmi8658_ff - imu_ff),
                     (unsigned)battery_adc_mv(), gp_board_usb_power() ? " 5V" : "", gp_board_charger_pin(),
                     (unsigned)c2_dock_mv, c2_dock ? " docked" : "");
    frames = hjc_stick_frames;
    bad = hjc_stick_bad;
    imu_ff = qmi8658_ff;
    return n;
}

#endif
