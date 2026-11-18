/*
 * erm_rumble.h: ERM rumble motors on MCPWM hardware pins, 25 kHz, driven as
 * stock GameSir firmware does (C2 0x1E68CF6..0x1E68E82): an intensity level
 * 0..ERM_LEVELS-1 picks per motor a duty range min..max and a start "kick".
 * When a motor starts it is driven at the kick duty for the kick time, then
 * at min + amp * (max - min) / 255; amplitude 0 stops it and re-arms the
 * kick. Level 0 is off. Duty in 1/10000.
 *
 * Off is the pin as a plain GPIO driven low, not a 0 % PWM: the PWM output
 * is detached (H_EN off) so a stopped timer can't leave the pin high.
 */
#ifndef ERM_RUMBLE_H
#define ERM_RUMBLE_H

#include "typedef.h"
#include "asm/mcpwm.h"

#define ERM_LEVELS          5

struct erm_motor {
    u8 pin;
    pwm_ch_num_type ch;
    pwm_timer_num_type timer;
    struct { u16 kick, kick_ms, min, max; } lvl[ERM_LEVELS];
};

void erm_rumble_init(const struct erm_motor *m, int n);
/* Every report tick (dt_ms since the last call). */
void erm_rumble_update(int i, u8 amp, u8 level, u16 dt_ms);
void erm_rumble_rekick(void);           /* start every running motor with its kick again */

#endif
