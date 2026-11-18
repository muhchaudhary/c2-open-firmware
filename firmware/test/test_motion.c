/* test_motion.c: gyro bias learning (core/gp_motion.c), on a fake clock.
 * Raw units: gyro 16 / dps, accel 8192 / g. */
#include <math.h>
#include "test.h"
#include "gp_motion.h"

static unsigned rng = 1;
static int noise(int pp)                                 /* -pp/2 .. pp/2, deterministic */
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % (pp + 1)) - pp / 2;
}

/* secs of samples every 4 ms: gyro = bias + turn rates, gravity tilted by
 * pitch over time, the noise measured on the pad at rest (gyro 6, accel 25
 * p-p) unless given. */
static void run(double secs, int bias, double yaw_dps, double pitch_dps, int accel_pp)
{
    static double ang;
    for (int k = 0; k < secs * 250; k++) {
        ang += pitch_dps * 0.004;
        double r = ang * M_PI / 180;
        s16 a[3] = { -414 + noise(accel_pp), (s16)(-1748 + 8000 * sin(r)) + noise(accel_pp), 7834 + noise(accel_pp) };
        s16 g[3] = { bias + noise(6) + (s16)(pitch_dps * 16), bias + noise(6), bias + noise(6) + (s16)(yaw_dps * 16) };
        gp_motion_update(a, g);
        fake_ms += 4;
    }
}

static int out(int axis)                                 /* gyro output, averaged a little */
{
    return gp_motion[3 + axis];
}

static void first_bias_learned_resting(void)
{
    run(1.4, 51, 0, 0, 25);
    CHECK_NEAR(out(0), 51, 4);                           /* not before 1.5 s */
    run(0.3, 51, 0, 0, 25);
    CHECK_NEAR(out(0), 0, 4);
    CHECK_NEAR(out(1), 0, 4);
    CHECK_NEAR(out(2), 0, 4);
    CHECK(fake_log_has("motion: gyro bias 0 0 0 -> "));
}

static void slow_drift_followed(void)
{
    for (int b = 52; b <= 62; b++) {
        run(1.6, b, 0, 0, 25);
    }
    CHECK_NEAR(out(0), 0, 4);                            /* following, 1.5 dps steps at most */
}

static void slow_turn_refused(void)
{
    fake_log_clear();
    run(4.6, 62, 2, 0, 25);                              /* 2 dps yaw, pad resting on a turntable */
    CHECK_NEAR(out(2), 32, 4);                           /* the turn still shows */
    CHECK_NEAR(out(0), 0, 4);
    CHECK(fake_log_has("far from the bias"));
    CHECK_EQ(fake_log_count, 1);                         /* logged once, not every window */
}

static void slow_tilt_refused(void)
{
    fake_log_clear();
    run(4, 62, 0, 0.5, 25);                              /* 0.5 dps pitch: gravity moves */
    CHECK_NEAR(out(0), 8, 4);                            /* 0.5 dps = 8 raw, still reported */
    CHECK(!fake_log_has("gyro bias"));
}

static void held_in_hand_not_learned(void)
{
    fake_log_clear();
    run(4, 70, 0, 0, 300);                               /* hand tremor on the accelerometer */
    CHECK_NEAR(out(0), 8, 4);
    CHECK(!fake_log_has("gyro bias"));
}

static void persistent_change_taken(void)
{
    fake_log_clear();
    run(4.4, 110, 0, 0, 25);                             /* 3 dps step: 2 windows */
    CHECK(!fake_log_has("taken"));
    run(3.2, 110, 0, 0, 25);                             /* 4 windows in a row (6 s) */
    CHECK(fake_log_has("taken"));
    CHECK_NEAR(out(0), 0, 4);
}

static void stored_bias_is_the_start(void)
{
    s16 cal[3] = { 0, 0, 0 };
    gp_motion_set_bias(cal);
    fake_log_clear();
    run(3, 51, 0, 0, 25);                                /* far from the stored one: not at once */
    CHECK_NEAR(out(0), 51, 4);
    run(4.6, 51, 0, 0, 25);                              /* ... but when it persists */
    CHECK_NEAR(out(0), 0, 4);
}

static void gap_restarts_window(void)
{
    run(1, 70, 0, 0, 25);
    fake_ms += 300;                                      /* no samples for 300 ms */
    run(1, 70, 0, 0, 25);
    CHECK_NEAR(out(0), 19, 4);                           /* no window spans the gap */
}

static void raw_kept_for_calibration(void)
{
    s16 a[3] = { 0, 0, 8192 }, g[3] = { 100, -200, 300 };
    gp_motion_update(a, g);
    CHECK_EQ(gp_motion_gyro_raw[0], 100);
    CHECK_EQ(gp_motion_gyro_raw[2], 300);
    CHECK_EQ(gp_motion[2], 8192);                        /* accel as measured */
}

static void output_saturates(void)
{
    s16 cal[3] = { -100, 0, 0 };
    gp_motion_set_bias(cal);
    s16 a[3] = { 0, 0, 8192 }, g[3] = { 32767, 0, 0 };
    gp_motion_update(a, g);
    CHECK_EQ(gp_motion[3], 32767);                       /* not wrapped to negative */
}

int main(void)
{
    printf("test_motion\n");
    RUN(first_bias_learned_resting);
    RUN(slow_drift_followed);
    RUN(slow_turn_refused);
    RUN(slow_tilt_refused);
    RUN(held_in_hand_not_learned);
    RUN(persistent_change_taken);
    RUN(stored_bias_is_the_start);
    RUN(gap_restarts_window);
    RUN(raw_kept_for_calibration);
    RUN(output_saturates);
    return t_done("test_motion");
}
