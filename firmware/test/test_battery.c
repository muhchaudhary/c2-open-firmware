/* test_battery.c: battery percent and "full" (drivers/battery_adc.c), on a
 * fake clock and ADC, with the C2's measured tables. */
#include "test.h"
#include "battery_adc.h"
#include "app_main.h"

#define CH 1

static const struct battery_point on_battery[] = {
    { 4128, 100 }, { 4052, 95 }, { 4023, 90 }, { 3985, 85 }, { 3940, 80 },
    { 3904, 75 }, { 3872, 70 }, { 3831, 65 }, { 3781, 60 }, { 3727, 55 },
    { 3685, 50 }, { 3656, 45 }, { 3631, 40 }, { 3614, 35 }, { 3595, 30 },
    { 3573, 25 }, { 3549, 20 }, { 3516, 15 }, { 3474, 10 }, { 3432, 5 },
    { 3318, 0 },
};
static const struct battery_point charging[] = {
    { 4203, 99 }, { 4185, 90 }, { 4167, 80 }, { 4149, 70 }, { 4113, 60 },
    { 4075, 50 }, { 3969, 40 }, { 3875, 30 }, { 3826, 20 }, { 3751, 10 },
    { 3604, 0 },
};
static int charger_done_pin;
static int charger_done(void)
{
    return charger_done_pin;
}
static const struct battery_adc_cfg cfg = {
    .adc_ch = CH, .divider = 2,
    .on_battery = on_battery, .on_battery_n = ARRAY_SIZE(on_battery),
    .charging = charging, .charging_n = ARRAY_SIZE(charging),
    .charger_done = charger_done, .warn_mv = 3400, .off_mv = 3300,
};

static struct gp_battery b;

/* secs at mv (the battery), a reading every step_ms (the poll: 50). */
static void run(double secs, int mv, int usb, int step_ms)
{
    fake_adc_mv[CH] = mv / 2;
    for (int t = 0; t < secs * 1000; t += step_ms) {
        battery_adc_update(usb, &b);
        fake_ms += step_ms;
    }
}

static void boot_and_table(void)
{
    battery_adc_init(&cfg);
    CHECK_EQ(app_var.warning_tone_v, 340);               /* the SDK's thresholds, 10 mV units */
    CHECK_EQ(app_var.poweroff_tone_v, 330);
    run(5, 3700, 0, 50);
    CHECK_NEAR(b.mv, 3700, 2);
    CHECK_EQ(b.plug, 4);                                 /* on battery */
    CHECK_EQ(b.charging, 0);
    CHECK_EQ(b.percent, 52);                             /* between 3685 (50) and 3727 (55) */
}

static void percent_moves_freely_in_the_first_minute(void)
{
    run(40, 3800, 0, 50);                                /* e.g. a reading low at boot, then right */
    CHECK(b.percent > 55);
}

static void on_battery_only_falls(void)
{
    run(30, 3800, 0, 50);                                /* past the first minute */
    u8 p = b.percent;
    run(120, 3900, 0, 50);                               /* the voltage recovers (rumble stopped) */
    CHECK_EQ(b.percent, p);                              /* the percent doesn't */
    run(200, 3600, 0, 50);
    CHECK(b.percent < p);
}

static void rumble_dip_ignored(void)
{
    run(200, 3700, 0, 50);
    u8 p = b.percent;
    run(1, 3400, 0, 50);                                 /* a second of motors at full */
    run(5, 3700, 0, 50);
    CHECK(p - b.percent <= 1);
}

static void same_by_clock_not_calls(void)
{
    /* Callers run at different rates (tick, poll): the slow average goes by the clock. */
    run(300, 3600, 0, 4);
    u8 fast = b.percent;
    run(300, 3600, 0, 50);
    CHECK_NEAR(b.percent, fast, 1);
}

static void plug_in_no_jump(void)
{
    run(300, 3685, 0, 50);
    u8 p = b.percent;
    run(10, 3969, 1, 50);                                /* 5 V: the charge current lifts the voltage */
    CHECK_EQ(b.plug, 2);                                 /* charging */
    CHECK_EQ(b.charging, 1);
    CHECK(b.percent >= p);                               /* never down at the plug */
    CHECK(b.percent < 100);
    run(600, 4180, 1, 50);
    CHECK(b.percent >= 85 && b.percent <= 99);           /* charging, not full: 99 at most */
}

static void full_from_charger_pin(void)
{
    charger_done_pin = 1;
    run(4.5, 4200, 1, 50);
    CHECK_EQ(b.plug, 2);                                 /* the pin must hold 5 s */
    run(1, 4200, 1, 50);
    CHECK_EQ(b.plug, 3);                                 /* full */
    CHECK_EQ(b.percent, 100);
    CHECK_EQ(b.charging, 0);
    charger_done_pin = 0;                                /* topping up again */
    run(6, 4200, 1, 50);
    CHECK_EQ(b.plug, 2);
    CHECK_EQ(b.percent, 100);                            /* doesn't drop at the restart */
}

static void unplug(void)
{
    run(1, 4150, 0, 50);
    CHECK_EQ(b.plug, 4);
    CHECK_EQ(b.charging, 0);
}

static void ends_of_the_table(void)
{
    battery_adc_init(&cfg);
    run(400, 4300, 0, 50);
    CHECK_EQ(b.percent, 100);
    run(400, 3000, 0, 50);
    CHECK_EQ(b.percent, 0);
}

int main(void)
{
    printf("test_battery\n");
    RUN(boot_and_table);
    RUN(percent_moves_freely_in_the_first_minute);
    RUN(on_battery_only_falls);
    RUN(rumble_dip_ignored);
    RUN(same_by_clock_not_calls);
    RUN(plug_in_no_jump);
    RUN(full_from_charger_pin);
    RUN(unplug);
    RUN(ends_of_the_table);
    return t_done("test_battery");
}
