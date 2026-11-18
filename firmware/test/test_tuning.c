/* test_tuning.c: stick and trigger shaping (core/gp_tuning.c) with the
 * active profile's settings. */
#include <math.h>
#include "test.h"
#include "gamepad.h"
#include "gp_settings.h"
#include "gp_tuning.h"

#define FULL 32767

static struct gp_profile *prof(void)
{
    return &GP_ACTIVE_PROFILE;
}

static void reset(void)
{
    gp_settings_set_defaults(1);
}

static long radius(s16 x, s16 y)
{
    return lround(sqrt((double)x * x + (double)y * y));
}

static void stick(int x, int y, s16 *ox, s16 *oy)
{
    *ox = x;
    *oy = y;
    gp_tune_stick(ox, oy);
}

static void stick_default_deadzone(void)
{
    reset();                                             /* 4 % inner dead zone */
    s16 x, y;
    stick(FULL * 4 / 100, 0, &x, &y);
    CHECK_EQ(x, 0);
    stick(0, -FULL * 3 / 100, &x, &y);
    CHECK_EQ(y, 0);
    stick(0, 0, &x, &y);
    CHECK(x == 0 && y == 0);
    stick(FULL * 5 / 100, 0, &x, &y);                    /* just past: small, not a jump */
    CHECK(x > 0 && x < FULL * 2 / 100);
    stick(FULL, 0, &x, &y);
    CHECK_EQ(x, FULL);
    stick(0, -FULL, &x, &y);
    CHECK_EQ(y, -FULL);
    stick(FULL / 2, 0, &x, &y);                          /* rescaled past the dead zone */
    CHECK_NEAR(x, (FULL / 2 - FULL * 4 / 100) * (long)FULL / (FULL - FULL * 4 / 100), 3);
}

static void stick_radial_direction_kept(void)
{
    reset();
    s16 x, y;
    stick(20000, -10000, &x, &y);
    CHECK_NEAR((long)x * -10000, (long)y * 20000, 20000L * 3);   /* same direction */
    CHECK(x > 0 && y < 0);
    /* The dead zone is on the radius: a diagonal under it is zero on both axes. */
    stick(FULL * 3 / 100, FULL * 2 / 100, &x, &y);
    CHECK(x == 0 && y == 0);
}

static void stick_corner_clamped_to_circle(void)
{
    reset();
    s16 x, y;
    stick(FULL, FULL, &x, &y);                           /* a square gate's corner */
    CHECK(radius(x, y) <= FULL + 2);
    CHECK_NEAR(x, y, 1);
    stick(-FULL, FULL, &x, &y);
    CHECK(radius(x, y) <= FULL + 2);
    CHECK(x < 0 && y > 0);
}

static void stick_outer_deadzone(void)
{
    reset();
    prof()->stick_outer = 10;
    s16 x, y;
    stick(FULL * 90 / 100, 0, &x, &y);
    CHECK_NEAR(x, FULL, 2);                              /* full this far before the edge */
    stick(FULL * 95 / 100, 0, &x, &y);
    CHECK_EQ(x, FULL);
}

static void stick_anti_deadzone(void)
{
    reset();
    prof()->stick_anti_deadzone = 20;
    s16 x, y;
    stick(FULL * 5 / 100, 0, &x, &y);                    /* just past the dead zone: starts at 20 % */
    CHECK_NEAR(x, FULL * 20 / 100, FULL / 100);
    stick(FULL * 3 / 100, 0, &x, &y);                    /* inside: still zero */
    CHECK_EQ(x, 0);
    stick(FULL, 0, &x, &y);
    CHECK_EQ(x, FULL);
}

static void stick_curves(void)
{
    s16 x, y;
    double t = 0.5;                                      /* half way past the dead zone */
    int in = (int)(FULL * 4 / 100 + t * (FULL - FULL * 4 / 100));
    for (int c = 0; c <= 3; c++) {
        reset();
        prof()->stick_curve = c;
        stick(in, 0, &x, &y);
        double want = c == 0 ? t : c == 1 ? pow(t, 1.5) : c == 2 ? t * t : t * t * t;
        CHECK_NEAR(x, lround(want * FULL), FULL / 200);
        stick(FULL, 0, &x, &y);
        CHECK_EQ(x, FULL);                               /* every curve still reaches full */
    }
}

static void stick_zero_deadzone(void)
{
    reset();
    prof()->stick_deadzone = 0;
    s16 x, y;
    stick(1, 0, &x, &y);
    CHECK_EQ(x, 1);
    stick(0, 0, &x, &y);                                 /* r = 0: no division by zero */
    CHECK(x == 0 && y == 0);
}

static void stick_extreme_settings(void)
{
    reset();
    prof()->stick_deadzone = 50;
    prof()->stick_outer = 30;
    prof()->stick_anti_deadzone = 50;
    prof()->stick_curve = 3;
    s16 x, y;
    for (int v = -FULL; v <= FULL; v += 997) {
        stick(v, v / 3, &x, &y);
        CHECK(radius(x, y) <= FULL + 2);
    }
}

static s16 trig(int v)
{
    s16 t = v;
    gp_tune_trigger(&t);
    return t;
}

static void trigger_default_identity(void)
{
    reset();
    for (int v = -32768; v <= 32767; v += 4099) {
        CHECK_EQ(trig(v), v);
    }
    CHECK_EQ(trig(32767), 32767);
    CHECK_EQ(trig(-32768), -32768);
}

static void trigger_deadzone_and_outer(void)
{
    reset();
    prof()->trigger_deadzone = 10;
    prof()->trigger_outer = 10;
    int at = -32768 + 65535 * 10 / 100;
    CHECK_EQ(trig(at), -32768);
    CHECK(trig(at + 700) > -32768);
    CHECK(trig(at + 700) < -32768 + 2000);               /* starts from zero, no jump */
    CHECK_NEAR(trig(-32768 + 65535 * 90 / 100), 32767, 2);
    CHECK_EQ(trig(32767), 32767);
    CHECK_NEAR(trig(0), 0, 100);                         /* half way is still about half */
}

static void trigger_hair(void)
{
    reset();
    prof()->hair_trigger = 1;
    CHECK_EQ(trig(-32768 + 65535 * 9 / 100), -32768);    /* at least 10 %: a resting finger doesn't fire it */
    CHECK_EQ(trig(-32768 + 65535 * 11 / 100), 32767);
    prof()->trigger_deadzone = 30;                       /* a larger dead zone wins */
    CHECK_EQ(trig(-32768 + 65535 * 25 / 100), -32768);
    CHECK_EQ(trig(-32768 + 65535 * 31 / 100), 32767);
}

static void active_profile_used(void)
{
    reset();
    gp_settings.prof[2].stick_deadzone = 50;
    s16 x, y;
    stick(FULL * 40 / 100, 0, &x, &y);
    CHECK(x != 0);                                       /* profile 0 active: 4 % */
    gp_settings.profile = 2;
    stick(FULL * 40 / 100, 0, &x, &y);
    CHECK_EQ(x, 0);
}

int main(void)
{
    printf("test_tuning\n");
    RUN(stick_default_deadzone);
    RUN(stick_radial_direction_kept);
    RUN(stick_corner_clamped_to_circle);
    RUN(stick_outer_deadzone);
    RUN(stick_anti_deadzone);
    RUN(stick_curves);
    RUN(stick_zero_deadzone);
    RUN(stick_extreme_settings);
    RUN(trigger_default_identity);
    RUN(trigger_deadzone_and_outer);
    RUN(trigger_hair);
    RUN(active_profile_used);
    return t_done("test_tuning");
}
