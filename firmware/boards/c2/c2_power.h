/*
 * c2_power.h: the GameSir C2's power, wake-up and charging settings,
 * decompiled from the stock app v3.56, in the terms of the SDK's board file
 * (apps/hid/board/br23/board_ac635n_demo.c: power_param, wk_param,
 * charge_data, board_power_init, power_set_mode). Confidence and stock
 * addresses as in c2_pins.h.
 *
 * Reading notes:
 *  - The P33 (PMU) helpers: P33_CON_SET 0x1E6E9AA, p33_rx_1byte 0x1E6E920,
 *    p33_tx_1byte 0x1E6E968, p33_and_1byte 0x1E6EA96, all in the boot-RAM
 *    block. Stock calls them as 0x2006D32 / 0x2006CA8 / 0x2006CF0 /
 *    0x2006E1E: 0x2000000 + RAM address is an alias of the boot-RAM code,
 *    so the "OS core" calls are calls into app.bin's RAM block.
 *  - Stock's VDDIOM level names (strings next to "VDDIOM_VOL_20V") are the
 *    older enum 2.0 2.2 2.4 2.6 2.8 3.0 3.2 3.6 V; this SDK's enum is
 *    2.2 2.4 2.6 2.8 3.0 3.2 3.4 3.6 V. Translate by voltage, not index.
 */
#ifndef C2_POWER_H
#define C2_POWER_H

/* ---- power_param (struct low_power_param), passed to power_init 0x1E5242A --
 * Two copies, chosen at startup 0x1E19C64 by the 5 V detect (0x1E34A26):
 *   0x1E5EB7C "wire power param set"     (5 V present): vddiom_lev 6
 *   0x1E5EB98 "wireless power param set" (on battery):  vddiom_lev 4
 * otherwise identical (sure):
 *   osc_type 2 (OSC_TYPE_BT_OSC), btosc_hz 24000000, delay_us 240
 *   (240 MHz system clock), config 0 (no powerdown in sniff),
 *   btosc_disable 0, dcdc_port 0, vddiow_lev 2, all keeps 0. */
#define C2_BTOSC_HZ             24000000
#define C2_SYS_HZ               240000000
#define C2_VDDIOM_WIRED_MV      3200        /* stock level 6 (old enum) -> SDK VDDIOM_VOL_32V */
#define C2_VDDIOM_WIRELESS_MV   2800        /* stock level 4 (old enum) -> SDK VDDIOM_VOL_28V */
#define C2_VDDIOW_MV            2800        /* level 2 in both enums: VDDIOW_VOL_28V */
/* Startup trim 0x1E19DB8: VDDIO is measured with the ADC ("vddio_vol %d
 * (mv)"); below 3400 mV VDDIOM goes to level 7 (3.6 V). power_init itself
 * sets VDDIOW to level 1 (2.4 V) first, then VDDIOM = max(vddiom_lev, a
 * table entry chosen by vddiow_lev) (0x1E526DE..0x1E526F2). */

/* ---- Regulator mode ---------------------------------------------------------
 * power_set_mode 0x1E42276 (called from board init 0x1E19FA8 and the
 * shutdown path 0x1E429CC) ignores its argument (LTO folded the mode in): if
 * the current mode b[0x1FF18] is 3 (DCDC15) it steps VDC13 4 -> 3, then
 * stores 2. Sure: the C2 runs on PWR_LDO15 (no DC-DC). */
#define C2_POWER_MODE           PWR_LDO15

/* ---- Wake-up (struct wakeup_param at 0x1E5B2E0, set as 0xF388 by 0x1E5037E,
 * power_wakeup_init 0x1E4275C) ---------------------------------------------- */
/* filter 0; port[1] (RAM 0xA498): PB11, pull-up, falling edge, BLUETOOTH_RESUME */
#define C2_WAKE_HOME            IO_PORTB_11
/* port[2] (RAM 0xA49D): PC03, no pulls, rising edge, BLUETOOTH_RESUME */
#define C2_WAKE_5V              IO_PORTC_03
/* charge = {BLUETOOTH_RESUME} (0x1E5B31C), sub = {BLUETOOTH_RESUME} (0x1E5B31D);
 * other ports, rtc ports, alarm, lvd: none. Sure. */

/* ---- 5 V / charging ----------------------------------------------------------
 * The chip's built-in charger is not used: the app only ever clears
 * CHARGE_EN and CHGBG_EN (P3_CHG_CON0 bits 0, 1, in the shutdown path
 * 0x1E42A10) and never writes the full-voltage / current register
 * P3_CHG_CON1. So charging is done by an external IC: TCFG_CHARGE_ENABLE 0
 * (the SDK then closes the fast charger itself). Likely.
 *
 * C2_5V_DETECT (PC03): high = 5 V present (sure: the "wire" power param, the
 * rising-edge wake-up, and "on battery" = low at 0x1E35698); a mode flag
 * b[0xEC8C] == 1 overrides it. The charging flag b[0xF2F8] (SInput's
 * Charging bit) is set while 5 V is present (0x1E35A44) and cleared when 5 V
 * goes away (0x1E359EE) or when the battery has read >= 4000 mV for 300
 * scans ("full", 0x1E35A6A). */
#define C2_5V_DETECT            IO_PORTC_03

/* ---- Reset and low-voltage detect: from the boot config (isd_config.ini),
 * which the rewrite keeps --------------------------------------------------
 * RESET = PB01_00_0: long-press reset pin PB01, time 0: off (PB01 is the
 * battery sense input). Stock code 0x1E19CC6 only clears P3_PINR_CON bit 2
 * when P3_PORT_SEL10 == 58. For the SDK: gpio_longpress_pin0_reset_config(
 * IO_PORTB_01, 0, 0), as the demo board does. Likely.
 * VLVD = 4: VDDIO low-voltage detect at 2.2 V (isd_config_rule.c's table;
 * note p33.h's VLVD_SEL_* enum starts at 1.9 V, one step off). */

/* ---- Battery ------------------------------------------------------------------
 * PB01 through a 1:2 divider (ADC slot 3, see c2_pins.h); battery percent at
 * b[0xA503] over 3.25..4.10 V (archive notes, not re-checked here). The
 * low-battery sleep path is the event dispatcher 0x1E35700 ("set low bat
 * enter sleep flag"); its voltage threshold: open. */

#endif
