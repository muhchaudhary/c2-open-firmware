/* test_hall_trigger.c: an analog hall trigger (drivers/hall_trigger.c), on
 * a fake clock and ADC. The value falls as the trigger is pressed. */
#include "test.h"
#include "hall_trigger.h"

#define CH      3
#define REST    485

static struct hall_trigger new_trigger(void)
{
    struct hall_trigger t = { .name = "LT", .adc_ch = CH, .span_min = 300 };
    hall_trigger_init(&t);
    return t;
}

/* Read for ms milliseconds at adc, a read every 4 ms; the last output. */
static int hold(struct hall_trigger *t, int adc, int ms, int endstop)
{
    int v = 0;
    fake_adc_value[CH] = adc;
    for (int k = 0; k < ms; k += 4) {
        v = hall_trigger_read(t, endstop);
        fake_ms += 4;
    }
    return v;
}

static void rest_learned_by_time(void)
{
    struct hall_trigger t = new_trigger();
    u32 t0 = fake_ms;
    fake_adc_value[CH] = REST;
    while (!t.rest_done && fake_ms - t0 < 2000) {
        CHECK_EQ(hall_trigger_read(&t, 0), -32768);      /* released while learning */
        fake_ms += 4;
    }
    /* 8 reads to fill the average, then 500 ms of rest. */
    CHECK(fake_ms - t0 >= 500 && fake_ms - t0 <= 500 + 8 * 4 + 8);
    CHECK_EQ(t.rest, REST);
}

static void travel_scaled(void)
{
    struct hall_trigger t = new_trigger();
    hold(&t, REST, 700, 0);
    CHECK_EQ(hold(&t, REST, 100, 0), -32768);
    CHECK_EQ(hold(&t, REST - 10, 100, 0), -32768);       /* inside the dead zone (12) */
    CHECK_EQ(hold(&t, REST - 300, 100, 0), 32767);       /* span_min: full */
    int half = hold(&t, REST - (12 + (300 - 12) / 2), 100, 0);
    CHECK_NEAR(half, 0, 300);
    CHECK_EQ(hold(&t, REST - 420, 100, 0), 32767);       /* further: the span grows ... */
    CHECK_EQ(t.span, 420);
    CHECK(hold(&t, REST - 300, 100, 0) < 32767);         /* ... and 300 is no longer full */
}

static void endstop_full_and_span(void)
{
    struct hall_trigger t = new_trigger();
    hold(&t, REST, 700, 0);
    CHECK_EQ(hold(&t, REST - 200, 100, 1), 32767);       /* the end-stop switch: full */
    CHECK_EQ(t.span, 200);                               /* and that travel is full scale */
    CHECK_EQ(hold(&t, REST - 200, 100, 0), 32767);
}

static void stopper_rezero_after_3s(void)
{
    /* A stopper put on after boot holds the trigger a little in: after 3 s
     * still there, that is the rest. */
    struct hall_trigger t = new_trigger();
    hold(&t, REST, 700, 0);
    int in = REST - 30;
    CHECK(hold(&t, in, 2900, 0) > -32768);               /* reads as a slight press for now */
    CHECK_EQ(t.rest, REST);
    hold(&t, in, 200, 0);
    CHECK_EQ(t.rest, in);
    CHECK_EQ(hold(&t, in, 100, 0), -32768);
    fake_log_clear();
}

static void moving_trigger_not_rezeroed(void)
{
    struct hall_trigger t = new_trigger();
    hold(&t, REST, 700, 0);
    for (int i = 0; i < 20; i++) {                       /* a light press that wobbles for 4 s */
        hold(&t, REST - 25 - (i & 1) * 15, 200, 0);
    }
    CHECK_EQ(t.rest, REST);
    hold(&t, REST - 100, 4000, 0);                       /* a real press held: past the re-zero band */
    CHECK_EQ(t.rest, REST);
}

static void stopper_removed(void)
{
    struct hall_trigger t = new_trigger();
    hold(&t, REST - 25, 700, 0);                         /* booted with a stopper on */
    CHECK_EQ(t.rest, REST - 25);
    hold(&t, REST, 100, 0);                              /* taken off: released further */
    CHECK_EQ(t.rest, REST);
}

static void calibrated(void)
{
    struct hall_trigger t = new_trigger();
    hall_trigger_set_cal(&t, 480, 470);
    CHECK(t.rest_done);
    fake_adc_value[CH] = 480 - 470;
    for (int i = 0; i < HALL_TRIGGER_AVG - 1; i++) {
        CHECK_EQ(hall_trigger_read(&t, 0), -32768);      /* the average isn't full yet: not a press */
        fake_ms += 4;
    }
    CHECK_EQ(hall_trigger_read(&t, 0), 32767);
    CHECK_EQ(hold(&t, 480, 100, 0), -32768);
    hall_trigger_clear_cal(&t);
    CHECK(!t.rest_done);
    CHECK_EQ(t.span, 300);
}

int main(void)
{
    printf("test_hall_trigger\n");
    RUN(rest_learned_by_time);
    RUN(travel_scaled);
    RUN(endstop_full_and_span);
    RUN(stopper_rezero_after_3s);
    RUN(moving_trigger_not_rezeroed);
    RUN(stopper_removed);
    RUN(calibrated);
    return t_done("test_hall_trigger");
}
