/*
 * battery_adc.h: a Li-ion battery measured on an ADC pin through a divider,
 * with an external charger IC that charges by itself while 5 V is present.
 *
 * Percent from measured voltage tables, one on battery and one while
 * charging (the charge current lifts the voltage: the same battery reads
 * ~70-300 mV higher on 5 V). The percent only falls on battery and only
 * rises on 5 V, so plugging in or out never makes it jump. "Full" comes
 * from the charger's status pin when the board has one, else from the
 * voltage holding at hold_mv (stock C2's rule, which fires well before the
 * charger is done).
 */
#ifndef BATTERY_ADC_H
#define BATTERY_ADC_H

#include "typedef.h"
#include "gamepad.h"

struct battery_point {
    u16 mv;
    u8  pct;
};

struct battery_adc_cfg {
    u32 adc_ch;                         /* AD_CH_* */
    u8  divider;                        /* battery mV = pin mV * divider */
    const struct battery_point *on_battery;  /* falling mV, falling %: 100 first, 0 last */
    u8  on_battery_n;
    const struct battery_point *charging;    /* the same, on 5 V */
    u8  charging_n;
    int (*charger_done)(void);          /* 1: the charger stopped (full), 0: charging; NULL: none */
    u16 hold_mv;                        /* without charger_done: "full" at least this ... */
    u16 hold_updates;                   /* ... for this many updates on 5 V */
    u16 warn_mv, off_mv;                /* the SDK's low-battery warning / power-off */
};

void battery_adc_init(const struct battery_adc_cfg *cfg);
/* Any rate (tick or poll): the averages, "full", the percent. */
void battery_adc_update(int usb_power, struct gp_battery *b);
u32  battery_adc_mv(void);              /* the fast running average, no update */

#endif
