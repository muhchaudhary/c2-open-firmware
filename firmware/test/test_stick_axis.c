/* test_stick_axis.c: one stick axis from raw values to -32767..32767
 * (drivers/stick_axis.c), on a fake clock. */
#include "test.h"
#include "stick_axis.h"

/* The C2's configuration (boards/c2/gamepad_c2.c). */
static const struct stick_axis_cfg cfg = {
    .span0 = 10000, .margin = 753, .lo = 1000, .hi = 31767, .still = 300, .recentre = 400, .avg = 1,
};
#define CENTRE 16400

/* Feed raw for ms milliseconds, a read every step ms; the last output. */
static int feed(struct stick_axis *a, int raw, int ms, int step)
{
    int v = 0;
    for (int t = 0; t < ms; t += step) {
        v = stick_axis_read(a, raw);
        fake_ms += step;
    }
    return v;
}

static void ready(struct stick_axis *a)
{
    stick_axis_init(a, "LX", &cfg);
    feed(a, CENTRE, 2600, 4);
}

static void centre_time_not_reads(void)
{
    /* 1.5 s settling + 1 s average, however often it is read. */
    for (int step = 2; step <= 8; step += 3) {
        struct stick_axis a;
        stick_axis_init(&a, "LX", &cfg);
        u32 t0 = fake_ms;
        while (a.phase != STICK_READY) {
            CHECK_EQ(stick_axis_read(&a, CENTRE), 0);   /* 0 while learning */
            fake_ms += step;
            if (fake_ms - t0 > 5000) {
                break;
            }
        }
        CHECK(fake_ms - t0 >= 2500 && fake_ms - t0 <= 2500 + 2 * step);
        CHECK_EQ(a.centre, CENTRE);
    }
}

static void implausible_values_ignored(void)
{
    struct stick_axis a;
    stick_axis_init(&a, "LX", &cfg);
    feed(&a, 500, 3000, 4);                              /* below lo: the chip isn't up */
    CHECK_EQ(a.phase, STICK_NEW);
    feed(&a, CENTRE, 2600, 4);
    CHECK_EQ(a.phase, STICK_READY);
}

static void lone_odd_reading_skipped(void)
{
    struct stick_axis a;
    stick_axis_init(&a, "LX", &cfg);
    feed(&a, CENTRE, 1700, 4);                           /* settled, now averaging */
    CHECK_EQ(a.phase, STICK_LEARNING);
    stick_axis_read(&a, CENTRE + 3000);                  /* one bad frame */
    stick_axis_read(&a, CENTRE + 3000);                  /* two */
    feed(&a, CENTRE, 1000, 4);
    CHECK_EQ(a.restarts, 0);
    CHECK_EQ(a.centre, CENTRE);
}

static void moved_while_learning_restarts(void)
{
    struct stick_axis a;
    stick_axis_init(&a, "LX", &cfg);
    feed(&a, CENTRE, 1700, 4);
    feed(&a, CENTRE + 3000, 12, 4);                      /* three in a row: it moved */
    CHECK_EQ(a.restarts, 1);
    CHECK_EQ(a.phase, STICK_LEARNING);
    u32 t0 = fake_ms;
    while (a.phase != STICK_READY && fake_ms - t0 < 3000) {
        stick_axis_read(&a, CENTRE + 3000);
        fake_ms += 4;
    }
    CHECK(fake_ms - t0 >= 1000);                         /* a full second again */
    CHECK_EQ(a.centre, CENTRE + 3000);
}

static void scaling(void)
{
    struct stick_axis a;
    ready(&a);
    CHECK_EQ(stick_axis_read(&a, CENTRE), 0);
    CHECK_EQ(stick_axis_read(&a, CENTRE + cfg.span0 - cfg.margin), 32767);   /* full a margin before the edge */
    CHECK_EQ(stick_axis_read(&a, CENTRE - (cfg.span0 - cfg.margin)), -32767);
    CHECK_NEAR(stick_axis_read(&a, CENTRE + (cfg.span0 - cfg.margin) / 2), 16383, 2);
}

static void span_learned_after_commit_time(void)
{
    struct stick_axis a;
    ready(&a);
    s32 span = a.span_pos;
    feed(&a, CENTRE + 12000, 196, 4);                    /* past the span: pending */
    CHECK_EQ(a.span_pos, span);
    feed(&a, CENTRE + 12000, 12, 4);
    CHECK_EQ(a.span_pos, 12000);                         /* 200 ms later: the stick's */
    CHECK_EQ(stick_axis_read(&a, CENTRE + 12000 - cfg.margin), 32767);
    CHECK_EQ(a.span_neg, cfg.span0);                     /* the other way untouched */
}

static void click_holds_learning(void)
{
    struct stick_axis a;
    ready(&a);
    for (int i = 0; i < 50; i++) {                       /* clicked while pushed: far past the travel */
        stick_axis_hold(&a);
        stick_axis_read(&a, CENTRE + 15000);
        fake_ms += 4;
    }
    feed(&a, CENTRE + 15000, 196, 4);                    /* released, still there: held off 200 ms */
    CHECK_EQ(a.span_pos, cfg.span0);
    feed(&a, CENTRE + 11000, 150, 4);                    /* learning again: pending, 200 ms to commit */
    CHECK_EQ(a.span_pos, cfg.span0);
    feed(&a, CENTRE + 11000, 100, 4);
    CHECK_EQ(a.span_pos, 11000);                         /* the 15000 read during the click never counted */
}

static void calibrated(void)
{
    struct stick_axis a;
    stick_axis_init(&a, "LX", &cfg);
    stick_axis_set_cal(&a, 16000, 11000, 12000);
    CHECK_EQ(a.phase, STICK_READY);                      /* no learning: usable at once */
    CHECK_EQ(stick_axis_read(&a, 16000), 0);
    CHECK_EQ(stick_axis_read(&a, 16000 + 11000 - cfg.margin), 32767);
    CHECK_EQ(stick_axis_read(&a, 16000 - (12000 - cfg.margin)), -32767);
    feed(&a, 16000 + 14000, 1000, 4);                    /* past the stored span: clamped, not learned */
    CHECK_EQ(a.span_pos, 11000);
    CHECK_EQ(stick_axis_read(&a, 30000), 32767);
    stick_axis_relearn(&a);                              /* calibrated: back to the stored centre */
    CHECK_EQ(a.centre, 16000);
    CHECK_EQ(a.phase, STICK_READY);
    stick_axis_clear_cal(&a);                            /* learn again */
    CHECK_EQ(a.phase, STICK_NEW);
    CHECK_EQ(a.span_pos, cfg.span0);
}

static void slow_recentre(void)
{
    struct stick_axis a;
    ready(&a);
    feed(&a, CENTRE + 200, 30000, 4);                    /* rests 200 off for 30 s */
    CHECK_NEAR(a.centre, CENTRE + 200, 10);
    feed(&a, CENTRE + 2000, 30000, 4);                   /* held deflected: not "drift" */
    CHECK_NEAR(a.centre, CENTRE + 200, 10);
}

static void average_length(void)
{
    struct stick_axis_cfg c6 = cfg;
    c6.avg = 6;
    struct stick_axis a;
    stick_axis_init(&a, "LX", &c6);
    feed(&a, CENTRE, 2600, 4);
    int full = CENTRE + cfg.span0 - cfg.margin, v = 0;
    for (int i = 1; i <= 6; i++) {
        v = stick_axis_read(&a, full);
        if (i < 6) {
            CHECK(v < 32767);                            /* the mean of 6 reads catches up */
        }
    }
    CHECK_EQ(v, 32767);
    c6.avg = 50;                                         /* more than the history holds: capped */
    stick_axis_init(&a, "LX", &c6);
    feed(&a, CENTRE, 2600, 4);
    for (int i = 0; i < STICK_AXIS_AVG; i++) {
        v = stick_axis_read(&a, full);
    }
    CHECK_EQ(v, 32767);
}

static void clock_wrap(void)
{
    u32 saved = fake_ms;
    fake_ms = 0xffffffffu - 1000;
    struct stick_axis a;
    stick_axis_init(&a, "LX", &cfg);
    int n = 0;
    while (a.phase != STICK_READY && n < 2000) {
        stick_axis_read(&a, CENTRE);
        fake_ms += 4;
        n++;
    }
    CHECK_NEAR(n, 2500 / 4, 2);
    fake_ms = saved;
}

static void radial(void)
{
    int x, y;
    stick_axis_radial(1000, 0, 2000, &x, &y);
    CHECK(x == 0 && y == 0);
    stick_axis_radial(32767, 32767, 0, &x, &y);          /* clamped to the circle */
    CHECK_NEAR(x, 23170, 2);
    CHECK_NEAR(y, 23170, 2);
    stick_axis_radial(-32767, 0, 1000, &x, &y);
    CHECK_EQ(x, -32767);
}

int main(void)
{
    printf("test_stick_axis\n");
    RUN(centre_time_not_reads);
    RUN(implausible_values_ignored);
    RUN(lone_odd_reading_skipped);
    RUN(moved_while_learning_restarts);
    RUN(scaling);
    RUN(span_learned_after_commit_time);
    RUN(click_holds_learning);
    RUN(calibrated);
    RUN(slow_recentre);
    RUN(average_length);
    RUN(clock_wrap);
    RUN(radial);
    return t_done("test_stick_axis");
}
