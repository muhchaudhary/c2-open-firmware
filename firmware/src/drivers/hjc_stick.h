/*
 * hjc_stick.h: the stick chip GameSir pads use (stock calls it "hjc723"). It
 * streams all four stick axes by itself over UART (RX only), 4-byte frames
 * [id, value hi, value lo, CRC-8 poly 0x31 over the first 3]; id 0xAA LX,
 * 0x55 LY, 0xCC RX, 0x33 RY; 15-bit values, rest ~16384. A value with bit 15
 * set is skipped (stock stores it as 0).
 *
 * Uses UART2 with a DMA ring (patch_sdk.py fixes the SDK driver's ring
 * index; its counters are gp_uart2_slips / gp_uart2_bogus).
 */
#ifndef HJC_STICK_H
#define HJC_STICK_H

#include "typedef.h"

enum { HJC_LX, HJC_LY, HJC_RX, HJC_RY };

int  hjc_stick_init(u8 rx_pin);         /* 0 = ok */
/* An axis: the mean of every value since the last call (one call per
 * report), or with hjc_stick_mean(0) the latest value only. 0 if there is
 * none yet (or since a baud change). */
int  hjc_stick_value(int axis, u16 *v);
void hjc_stick_mean(int on);                    /* default on */
/* Report tick: baud search. 1 = the rate changed: relearn the centres. */
int  hjc_stick_baud_check(void);
/* Poll: re-set the rate when the UART clock changed. */
void hjc_stick_clock_check(void);

/* Latency probe (log command "stickprobe"): per axis, the arrival time
 * (gp_us) of the first value at least thresh from centre (raw, the chip's
 * own direction); armed again once it is back within thresh / 2. take: 1,
 * the time and the side (+1 above the centre, -1 below) once per crossing. */
void hjc_probe_arm(int axis, u16 centre, u16 thresh);
void hjc_probe_off(void);
int  hjc_probe_take(int axis, u32 *t_us, int *dir);

/* Debug log counters. */
extern volatile u32 hjc_stick_frames;   /* good frames */
extern volatile u32 hjc_stick_bad;      /* CRC failures where a frame was due */
extern volatile u32 hjc_stick_glitches; /* values held back and never confirmed */

#endif
