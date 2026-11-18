/* gp_charging.c: see gp_charging.h.
 *
 * Stock's charging animation (C2 0x1E4404A, a state machine in the LED task;
 * colour setter 0x1E43D8C, fade engine 0x1E43EAE): the player LEDs fade in
 * one after another from the bottom one to the top, then Home, then all
 * fade out together, a pause, and again. Each fade is 100 steps of 8
 * animation ticks (the first player-to-player step 10); the pause 200
 * ticks. The dock's LED (slot 7, the chain's end) joins in at the end:
 * full in the fade-out with the rest, fading with them, dark in the pause
 * (stock 0x1E441DC). The colour is the battery level, picked at the start of each round
 * (b[0xA504] 0..4): red, orange, yellow, green, and green with "full", after
 * which stock stays dark (here: off). Other LEDs dark (stock clears the
 * grips). Stock's tick isn't pinned down; 2 ms (its timer base) is assumed:
 * ~10 s a round. */
#include "system/includes.h"
#include "asm/power/p33.h"
#include "gamepad.h"
#include "gp_leds.h"
#include "gp_power.h"
#include "gp_usb.h"
#include "gp_charging.h"
#include "gp_log.h"

extern unsigned long jiffies_msec(void);

#define CHG_TICK_MS         2
#define CHG_MAX             64          /* a full colour (stock 255): matches the other LEDs' brightness */
#define CHG_POLL_MS         10
static const u16 chg_ms[7] = {          /* per state: 5 fade-ins, the fade-out, the pause */
    100 * 8 * CHG_TICK_MS, 100 * 10 * CHG_TICK_MS, 100 * 8 * CHG_TICK_MS,
    100 * 8 * CHG_TICK_MS, 100 * 8 * CHG_TICK_MS, 100 * 8 * CHG_TICK_MS,
    200 * CHG_TICK_MS,
};
static u8 chg_slots[5];                 /* bottom player LED .. top, then Home */
static u8 chg_on, chg_state, chg_full, chg_home;
static u8 chg_col[3];
static u32 chg_t0;

static void chg_pick_colour(void)
{
    static const u8 col[5][3] = {
        { 255, 0, 0 }, { 255, 128, 0 }, { 255, 255, 0 }, { 0, 255, 0 }, { 0, 255, 0 },
    };
    struct gp_battery b;
    gp_board_battery(&b);
    int lvl = b.plug == 3 ? 4 : b.percent / 25;
    if (lvl > 4) {
        lvl = 4;
    }
    if (lvl == 4 && b.plug != 3) {
        lvl = 3;                                         /* 100 % but not held long enough yet */
    }
    for (int i = 0; i < 3; i++) {
        chg_col[i] = col[lvl][i] * CHG_MAX / 255;
    }
    chg_full = lvl == 4;
}

static void chg_off(void)
{
    gp_leds_dark();
    gp_board_led_power(0);
}

static void chg_full_off(void *priv)
{
    p33_tx_1byte(P3_WKUP_CPND, 0xff);
    power_set_soft_poweroff();                           /* stock stays dark; off is the same to see */
}

static void chg_poll(void *priv)
{
    if (!chg_on) {
        return;
    }
    u32 now = jiffies_msec();
    struct gp_battery b;
    gp_board_battery(&b);                                /* keeps the average and the "full" hold going */
    static u8 dock_div;
    if (++dock_div >= GP_POLL_MS / CHG_POLL_MS) {        /* the dock sense at its usual rate */
        dock_div = 0;
        gp_board_dock_poll();
    }
    static u32 log_ms;
    if (now - log_ms >= 10000) {                         /* the charge curve (tools/c2batlog.py) */
        log_ms = now;
        gp_log("charging: bat %u mV %u%% plug %u pb02 %d%s%s", b.mv, b.percent, b.plug,
               gp_board_charger_pin(), gp_board_usb_power() ? " 5V" : "", gp_board_docked() == 1 ? " docked" : "");
    }
    if (!gp_board_charge_power()) {
        gp_log("charging: unplugged or off the dock, off");
        chg_on = 0;
        chg_off();
        gp_power_soft_off();                             /* 5 V gone: the real soft-off */
        return;
    }
    /* Home: wait until it has been seen up (it may still be held from the
     * power-off), then a press and release restarts into normal use. */
    u8 home = (gp_board_buttons() & GP_BIT(GP_BTN_GUIDE)) != 0;
    if (chg_home == 0 && !home) {
        chg_home = 1;
    } else if (chg_home == 1 && home) {
        chg_home = 2;
    } else if (chg_home == 2 && !home) {
        gp_log("charging: Home, starting");
        p33_tx_1byte(P3_WKUP_CPND, 0xff);               /* so the next boot isn't "woken by USB" */
        cpu_reset();
        return;
    }
    u32 t = now - chg_t0;
    if (t >= chg_ms[chg_state]) {
        chg_t0 = now;
        t = 0;
        if (++chg_state > 6) {
            if (chg_full) {
                gp_log("charging: full, off");
                chg_off();
                chg_on = 0;                              /* this poll stops here */
                sys_timeout_add(NULL, chg_full_off, 300);   /* let the log line out over USB first */
                return;
            }
            chg_state = 0;
            chg_pick_colour();
        }
    }
    u32 p = t * 100 / chg_ms[chg_state];                 /* progress 0..99 % */
    memset(gp_led_rgb, 0, sizeof(gp_led_rgb));
    for (int k = 0; k < 5; k++) {
        u8 slot = chg_slots[k];
        u32 level = chg_state < 5 ? (k < chg_state ? 100 : k == chg_state ? p : 0)
                    : chg_state == 5 ? 100 - p : 0;
        for (int i = 0; i < 3; i++) {
            gp_led_rgb[slot][i] = chg_col[i] * level / 100;
        }
    }
    u8 dock = gp_board_info.leds.dock;
    if (dock != GP_LED_NONE && chg_state == 5) {
        for (int i = 0; i < 3; i++) {
            gp_led_rgb[dock][i] = chg_col[i] * (100 - p) / 100;
        }
    }
    gp_leds_send();
}

void gp_charging_start(void)
{
    if (chg_on) {
        return;
    }
    chg_on = 1;
    gp_usb_log_only();                                   /* the host sees the log, not a silent gamepad */
    /* Without Bluetooth nothing else keeps the system up: the SDK's idle
     * query let it drop into low-power sleep ~2 s in, taking USB and this
     * timer with it (2026-11-13: no animation, the log stopped). */
    gp_keep_awake = 1;
    /* The power-off path and the USB wake had the LED supply off, and the
     * LEDs stayed dark while the frames went out (2026-11-13, debug log). */
    gp_board_led_power(1);
    /* Home (the chain's first LED) flashed white for a moment before the
     * animation (2026-11-12): the chain comes up from off with no frame
     * until the first poll, 10 ms later. A dark frame as soon as the LEDs
     * are up. (Not c2_udelay: it takes TIMER0.) */
    for (volatile u32 i = 0; i < 20000; i++);
    gp_leds_dark();
    for (int k = 0; k < 4; k++) {
        chg_slots[k] = gp_board_info.leds.player[3 - k];
    }
    chg_slots[4] = gp_board_info.leds.home;
    chg_state = 0;
    chg_home = 0;
    chg_t0 = jiffies_msec();
    chg_pick_colour();
    gp_log("charging: animation, colour %u %u %u%s", chg_col[0], chg_col[1], chg_col[2],
           chg_full ? " (full)" : "");
    sys_timer_add(NULL, chg_poll, CHG_POLL_MS);
}
