/*
 * c2_pins.h: the GameSir C2's pin map, decompiled from the stock app v3.56
 * (fw-re/app_v356.bin). For the SDK board file of the firmware (docs/firmware-design.md,
 * phase 2). IO_PORTx_nn / AD_CH_* / IRQ names are the SDK's (br23).
 *
 * Confidence: "sure" = read directly from stock code with a matching SDK
 * driver or a debug string; "likely" = inferred; "open" = not resolved yet.
 * Every entry names the stock address it comes from.
 */
#ifndef C2_PINS_H
#define C2_PINS_H

/* ---- Buttons ------------------------------------------------------------
 * Stock scan 0x1E35AA8 reads each pin's IN bit (pull-up, active low) into
 * the raw key word b[0xEFC4] (= 0xEC80 + 836); its bits are listed here.
 * Names: the power-on debug dump 0x1E35DB2 ("power on <name> press") for
 * bits 0-7, 14-17, 19; sinput.c (back paddles = bits 8, 10) and the
 * joypad task for the rest. All set up in "input port init" 0x1E3421A as
 * input + pull-up + digital in. */
#define C2_KEY_UP        IO_PORTA_02   /* bit 0, sure */
#define C2_KEY_DOWN      IO_PORTA_04   /* bit 1, sure */
#define C2_KEY_LEFT      IO_PORTB_08   /* bit 2, sure */
#define C2_KEY_RIGHT     IO_PORTA_10   /* bit 3, sure */
#define C2_KEY_L1        IO_PORTC_07   /* bit 4, sure */
#define C2_KEY_R1        IO_PORTC_00   /* bit 5, likely (not in the dump; the slot between L1 and L3) */
#define C2_KEY_L3        IO_PORTC_01   /* bit 6, sure */
#define C2_KEY_R3        IO_PORTC_02   /* bit 7, sure */
#define C2_KEY_PADDLE_A  IO_PORTA_05   /* bit 8, sure: a back paddle (which side: open) */
#define C2_KEY_PADDLE_B  IO_PORTC_04   /* bit 10, sure: the other back paddle */
#define C2_KEY_SOUTH     IO_PORTA_00   /* bit 14, sure: "Cross" (A) */
#define C2_KEY_EAST      IO_PORTA_06   /* bit 15, sure: "Circule" (B) */
#define C2_KEY_WEST      IO_PORTA_07   /* bit 16, sure: "square" (X) */
#define C2_KEY_NORTH     IO_PORTA_13   /* bit 17, sure: "Triangle" (Y) */
#define C2_KEY_HOME      IO_PORTB_11   /* bit 18, likely: also the wake-up pin in every sleep / Switch2 wake path */
#define C2_KEY_SHARE     IO_PORTA_15   /* bit 19, sure: "Share" (View) */
#define C2_KEY_OPTIONS   IO_PORTA_14   /* bit 20, likely: Options (Menu) */
#define C2_KEY_CAPTURE   IO_PORTD_05   /* bit 21, sure (tested): the screenshot button below Home; stock also
                                        * enters pair mode when it is held 1 s while charging */
#define C2_KEY_M         IO_PORTB_07   /* bit 22, sure (tested): "M" (stock 0x1E49AF0: M + Up / Down = rumble level) */
#define C2_KEY_BIT22     C2_KEY_M
/* bits 30, 31: PR pins in the always-on domain (R3_PR_IN, 0x8098), read by
 * 0x1E507B8(1)/(0): the trigger end-stops. Stock 0x1E6ADA8 sets the LT / RT
 * digital bits (raw 12 / 13) from them and pins the trigger value at 1023.
 * Measured on the rewrite (pull-up inputs): both stay high at full trigger
 * travel, so they are not simple end-stops on this unit. Open. */
/* PB02: input + pull-up in "input port init" but not in the scan. Open. */

/* ---- Analog: GameSir's own ADC scan ----------------------------------------
 * The ADC interrupt 0x1E691D0 (boot-RAM code) stores JL_ADC->RES in
 * h[0xF5F0 + 2 * slot] and starts the next slot from the table at 0x1E5A6B0
 * ({slot, channel} x 7). Slot users: 0x1E49028 (triggers), 0x1E35896
 * (battery), 0x1E37AF8 / 0x1E683D2 (slot 2), 0x1E371EC / 0x1E45D64 (slot 4). */
#define C2_LT            IO_PORTB_03   /* slot 0, AD_CH_PB3 (6), sure: x4 into the LT state 0xEC80+3754 ("LTrigger min/max"); sets raw key bit 12.
                                        * Measured: 10-bit ~483 at rest, falls to 11 fully pressed */
#define C2_RT            IO_PORTB_10   /* slot 1, AD_CH_PB10 (9), sure: x4 into the RT state 0xEC80+3812; sets raw key bit 13.
                                        * Measured: ~490 at rest, falls to 11 fully pressed */
#define C2_AN_PA01       IO_PORTA_01   /* slot 2, AD_CH_PA1 (0), sure: the charging pedestal's 5 V sense, mV (vs VBG) x 151 / 100;
                                        * stock grades it <= 1600 / <= 4000 / above (0x1E37B04, 0x1E683E2): at boot, state 2
                                        * turns the pedestal power on (C2_PEDESTAL_PWR); a change later -> "pedestal change sleep" */
#define C2_VBAT_DIV      IO_PORTB_01   /* slot 3, AD_CH_PB1 (5), sure: battery, mV x 2 (1:2 divider), 100-sample mean, levels at 4000 / 3800 / 3450 mV */
#define C2_AN_PA12       IO_PORTA_12   /* slot 4, AD_CH_PA12 (4), sure: headset jack detect. At "joypad task init" (0x1E371EC)
                                        * x4 > 3711 sets f_AudioDevice; at runtime (0x1E45D64) x4 > 512 = headset present, a change
                                        * logs "Android Mode Headset Inster / Remove Reset!" and resets. Stock's shared audio code
                                        * (PA08, its "audio PA", powers the sticks on the C2); unused by the rewrite */
/* slot 5: AD_CH_PMU_VBG (the reference), slot 6: AD_CH_VBAT (0x50000F, internal 1/4 VBAT) */

/* ---- Sticks: an external chip ("hjc723") over UART ------------------------
 * Sure. Opened at 0x1E3771C by the startup handler 0x1E37676 with
 * uart_platform_data {tx PA02, rx PA03, 64 B ring at 0xF1A4, frame 4 B,
 * timeout 1 ms, callback 0x678E (RAM), baud 941176}; PA02/PA03 is hardware
 * group 8 of the UART pin tables (RAM 0xB7B9 / 0xB7C5). input_port_init is
 * called right after (0x1E3773C) and turns PA02 back into the D-pad Up input,
 * so only RX is used: the chip streams on its own.
 *
 * Frames (callback 0x1E6E406): 4 bytes, [id, value hi, value lo, crc].
 * crc = CRC-8 poly 0x31, init 0, MSB first, no final xor, over the first 3
 * bytes (0x1E4F85E, table 0x1E5A830). A value with bit 15 set is stored as 0.
 *   0xAA -> h[0x101E6] left X        0x55 -> h[0x101E8] left Y (used as 0x7FFF ^ v)
 *   0xCC -> h[0x101EA] right X (used as 0x7FFF ^ v)   0x33 -> h[0x101EC] right Y
 * stick_process 0x1E49332 (callers 0x1E49848 left, 0x1E4997E right) then
 * filters (6-sample history) and calibrates; centre 8192 ("ScanStickInit"). */
#define C2_STICK_RX      IO_PORTA_03   /* sure */
#define C2_STICK_BAUD    941176        /* what stock asks its driver for; on the wire it is ~500000
                                        * (the rewrite locks at 500000, tested; stock's divisor depends on
                                        * its UART clock). Values: 15-bit, rest ~16384, swing ~10000-12500 */

/* ---- LED chain (8 WS2812-style pixels, docs/sinput.md 1) -----------------
 * SPI1 port 'B' (JL_IOMAP->CON1 bit 4 = 1, 0x1E33C9E), DMA from the encoded
 * frame (0x1E34934). The SDK's cpu/br23/led_spi.c drives the same thing:
 * LED_SPI = JL_SPI1, LED_SPI_PORT = 'B'. */
#define C2_LED_DO        IO_PORTC_05   /* sure: SPI1 port B DO; idles high (0x1E34518) */

/* ---- IMU: QST QMI8658 at 0xD4, tested (stock's register writes 0x1E3784E..: soft
 * reset 0x60, CTRL1..CTRL9, data at 0x35; earlier notes said LSM6DS3) ------
 * Hardware IIC, port group c (JL_IOMAP->CON1 bits 18-19 = 2, 0x1E34AF0), the
 * SDK's hwiic_iomap {IO_PORTB_04, IO_PORTB_06}; HD set, pulls off; baud
 * clk / 800000 - 1. Its interrupt is set up at 0x1E34B9A (priority 3). */
#define C2_IIC_SCL       IO_PORTB_04   /* sure */
#define C2_IIC_SDA       IO_PORTB_06   /* sure */

/* ---- Rumble ---------------------------------------------------------------
 * Two mcpwm channels at 25 kHz, duty 0 at start (0x1E377A0): struct
 * pwm_platform_data {25000, ch, timer, 0, 0, h_pin, l_pin = -1, .., 1}.
 * Both are the SDK's hardware pins (pwm_hw_h_pin[4], [1]). Which motor is
 * which: open. */
#define C2_MOTOR_A       IO_PORTA_09   /* sure: pwm_ch4 / timer4 */
#define C2_MOTOR_B       IO_PORTB_00   /* sure: pwm_ch1 / timer1 */

/* ---- Power / other outputs ------------------------------------------------ */
#define C2_EXT_POWER     IO_PORTB_09   /* sure: "extend power", on when low (0x1E348FE); powers the grip LEDs (tested) */
#define C2_PA08_OUT      IO_PORTA_08   /* sure (tested): the stick chip's power, on when high. Stock calls it the "audio
                                        * power amplifier", low at boot, high a moment after a mode starts
                                        * (0x1E4185C / 0x1E484DA), low on sleep / reset paths */
#define C2_PEDESTAL_PWR  IO_PORTB_05   /* sure: "pedestal power" (the charging dock), on when high: "open pedestal power"
                                        * 0x1E35666 / "close pedestal power" 0x1E45FEE (flag b[0xE42C]). Low at init; stock turns
                                        * it on from the PA01 sense above; the rewrite keeps it off (no pedestal support) */
#define C2_5V_DETECT_PIN IO_PORTC_03   /* sure: 5 V present when high (c2_power.h) */

/* ---- Internal SPI flash: the SDK owns these, leave them alone --------------
 * spi_cs 0x1E6EB0A / spi_soft_* 0x1E6F2E0: PA13 or PD03 CS, PD00 CLK,
 * PD01 DO, PA14/PA15 data (the SDK's SPI0 port tables). PA13/PA14/PA15 are
 * also buttons above, so the flash runs on port A' = PD00/PD01/PD03. */

#endif
