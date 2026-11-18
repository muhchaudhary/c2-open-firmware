/* battery_adc.c: see battery_adc.h. The SDK's own low-battery warning /
 * power-off (app_power_manage.c) uses app_var's thresholds; the board's
 * values replace its defaults (2.4 / 2.2 V, far below a Li-ion's safe end).
 * patch_sdk.py points the SDK's get_vbat_level() at the same pin. */
#include "system/includes.h"
#include "asm/adc_api.h"
#include "app_main.h"
#include "battery_adc.h"

extern unsigned long jiffies_msec(void);

#define BAT_DONE_MS     5000            /* the charger pin must hold this long ... */
#define BAT_SETTLE_MS   3000            /* ... and 5 V this long, before "full" changes */
#define BAT_SEED_MS     3000            /* after the first reading: the ~30 s average follows the fast one */
#define BAT_WARM_MS     60000           /* ... and the percent moves both ways (no hold yet) */

static const struct battery_adc_cfg *bat;
static u32 bat_avg;                     /* mV << 6, fast running average (~64 updates) */
static u32 bat_slow;                    /* mV << 5, once a second: ~30 s, for the percent */
static u32 bat_sec_ms, bat_start_ms;
static u8  bat_pct = 0xff;              /* the percent reported */
static u8  bat_full, bat_5v, bat_done = 0xff;
static u32 bat_5v_ms, bat_done_ms;
static u16 bat_hold;

void battery_adc_init(const struct battery_adc_cfg *cfg)
{
    bat = cfg;
    adc_add_sample_ch(cfg->adc_ch);
    app_var.warning_tone_v = cfg->warn_mv / 10;          /* 10 mV units */
    app_var.poweroff_tone_v = cfg->off_mv / 10;
}

u32 battery_adc_mv(void)
{
    return bat_avg >> 6;
}

/* Linear between the table's points; above the first / below the last: theirs. */
static int bat_lookup(const struct battery_point *t, int n, u32 mv)
{
    if (mv >= t[0].mv) {
        return t[0].pct;
    }
    for (int i = 1; i < n; i++) {
        if (mv >= t[i].mv) {
            u32 dm = t[i - 1].mv - t[i].mv, dp = t[i - 1].pct - t[i].pct;
            return t[i].pct + ((mv - t[i].mv) * dp + dm / 2) / dm;
        }
    }
    return t[n - 1].pct;
}

void battery_adc_update(int usb_power, struct gp_battery *b)
{
    u32 now = jiffies_msec();
    u32 mv = adc_get_voltage(bat->adc_ch) * bat->divider;
    if (!bat_avg) {
        bat_avg = mv << 6;
        bat_slow = mv << 5;
        bat_sec_ms = now;
        bat_start_ms = now;
    } else {
        bat_avg += mv - (bat_avg >> 6);
    }
    mv = bat_avg >> 6;
    b->mv = mv;
    /* The percent's own average, by the clock (callers run at different
     * rates): rumble and radio dips of a second don't move it. */
    if (now - bat_sec_ms > 10000) {
        bat_sec_ms = now;                                /* not called for a while */
    }
    while (now - bat_sec_ms >= 1000) {
        bat_sec_ms += 1000;
        bat_slow += mv - (bat_slow >> 5);
    }
    /* Seeded from the fast average once it has settled, not from the first
     * reading: that one read low at boot (2026-11-16: 94 % at 4138 mV, and the
     * hold kept it there). */
    if (now - bat_start_ms < BAT_SEED_MS) {
        bat_slow = mv << 5;
    }
    u8 warm = now - bat_start_ms < BAT_WARM_MS;          /* the first minute: no hold */
    u32 slow = bat_slow >> 5;

    /* 5 V and "full" */
    if (usb_power != bat_5v) {
        bat_5v = usb_power;
        bat_5v_ms = now;
        bat_hold = 0;
        if (!usb_power) {
            bat_full = 0;
        }
    }
    if (usb_power) {
        if (bat->charger_done) {
            u8 d = bat->charger_done() ? 1 : 0;
            if (d != bat_done) {
                bat_done = d;
                bat_done_ms = now;
            }
            if (now - bat_done_ms >= BAT_DONE_MS && now - bat_5v_ms >= BAT_SETTLE_MS) {
                bat_full = d;                            /* the charger can start again: then charging */
            }
        } else if (mv >= bat->hold_mv) {
            if (bat_hold < bat->hold_updates) {
                bat_hold++;
            } else {
                bat_full = 1;
            }
        } else {
            bat_hold = 0;
        }
    }

    /* The percent: on battery it only falls, on 5 V it only rises (the
     * charge current's lift can't make it jump at the plug), 100 only once
     * the charger is done. */
    int pct;
    if (!usb_power) {
        pct = bat_lookup(bat->on_battery, bat->on_battery_n, slow);
        if (bat_pct != 0xff && pct > bat_pct && !warm) {
            pct = bat_pct;
        }
    } else if (bat_full) {
        pct = 100;
    } else {
        pct = bat_lookup(bat->charging, bat->charging_n, slow);
        if (pct > 99) {
            pct = 99;
        }
        if (bat_pct != 0xff && pct < bat_pct && !warm) {
            pct = bat_pct;                               /* may be 100: the charger started again */
        }
    }
    bat_pct = pct;
    b->percent = pct;
    if (!usb_power) {
        b->plug = 4;                                     /* on battery */
        b->charging = 0;
    } else {
        b->plug = bat_full ? 3 : 2;                      /* full / charging */
        b->charging = !bat_full;
    }
}
