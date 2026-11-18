/* erm_rumble.c: see erm_rumble.h. Duty is set through mcpwm_set_duty(ch,
 * timer, d), as stock (C2 0x1E46020). */
#include "system/includes.h"
#include "erm_rumble.h"

#define ERM_MAX             2

PWM_CH_REG *get_pwm_ch_reg(pwm_ch_num_type index);                        /* cpu/br23/mcpwm.c */
void mcpwm_set_duty(pwm_ch_num_type pwm_ch, pwm_timer_num_type timer_ch, u16 duty);

static const struct erm_motor *erm;
static u8  erm_n;
static u16 erm_ms[ERM_MAX];             /* time since the motor started (kick) */
static u16 erm_duty[ERM_MAX];           /* last duty set, 0 = detached */

void erm_rumble_init(const struct erm_motor *m, int n)
{
    erm = m;
    erm_n = n > ERM_MAX ? ERM_MAX : n;
    for (int i = 0; i < erm_n; i++) {
        struct pwm_platform_data pd = {
            .frequency = 25000,
            .pwm_ch_num = m[i].ch,
            .pwm_timer_num = m[i].timer,
            .duty = 0,
            .pwm_aligned_mode = pwm_edge_aligned,
            .h_pin = m[i].pin,
            .l_pin = (u8) -1,
            .complementary_en = 1,
        };
        mcpwm_init(&pd);
        get_pwm_ch_reg(m[i].ch)->ch_con0 &= ~BIT(2);    /* H_EN off: pin is GPIO (low) */
        gpio_direction_output(m[i].pin, 0);
    }
}

static void erm_set(int i, u16 duty)
{
    if (duty == erm_duty[i]) {
        return;
    }
    erm_duty[i] = duty;
    PWM_CH_REG *reg = get_pwm_ch_reg(erm[i].ch);
    if (duty == 0) {
        reg->ch_con0 &= ~BIT(2);                         /* detach: GPIO low */
    } else {
        mcpwm_set_duty(erm[i].ch, erm[i].timer, duty);
        reg->ch_con0 |= BIT(2);                          /* H_EN: PWM drives the pin */
    }
}

void erm_rumble_update(int i, u8 amp, u8 level, u16 dt_ms)
{
    if (i >= erm_n) {
        return;
    }
    const typeof(erm[0].lvl[0]) *l = &erm[i].lvl[level];
    u16 duty = 0;
    if (amp == 0 || l->max == 0) {
        erm_ms[i] = 0;
    } else if (erm_ms[i] <= l->kick_ms) {
        erm_ms[i] += dt_ms;
        duty = l->kick;
    } else {
        duty = l->min + (u32) amp * (l->max - l->min) / 255;
    }
    erm_set(i, duty);
}

void erm_rumble_rekick(void)
{
    for (int i = 0; i < ERM_MAX; i++) {
        erm_ms[i] = 0;
    }
}
