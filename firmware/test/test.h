/*
 * test.h: a minimal test harness for the host tests (firmware/test). Each
 * test program is one source file plus the firmware files it tests; main()
 * RUNs its tests and returns t_done().
 */
#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <stdlib.h>
#include "typedef.h"

extern int t_fail, t_checks;

#define CHECK(c) do { \
    t_checks++; \
    if (!(c)) { t_fail++; printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    t_checks++; \
    if (_a != _b) { t_fail++; printf("    FAIL %s:%d: %s == %s (%ld != %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)

#define CHECK_NEAR(a, b, tol) do { \
    long _a = (long)(a), _b = (long)(b); \
    t_checks++; \
    if (labs(_a - _b) > (long)(tol)) { t_fail++; \
        printf("    FAIL %s:%d: %s ~ %s (%ld vs %ld, tolerance %ld)\n", __FILE__, __LINE__, #a, #b, _a, _b, (long)(tol)); } \
} while (0)

#define RUN(name) do { printf("  %s\n", #name); name(); } while (0)

int t_done(const char *suite);

/* fake.c: the SDK and board pieces the firmware files call. */
extern u32 fake_ms;                     /* jiffies_msec() */
extern u32 fake_us_in_ms;               /* local_timer_us_time(): 0..1999 */
extern u32 fake_adc_value[16];          /* adc_get_value(ch), raw */
extern u32 fake_adc_mv[16];             /* adc_get_voltage(ch), mV at the pin */
extern int fake_store_fail;             /* gp_store_write fails */
void fake_store_clear(void);
int fake_store_put(u8 id, const void *buf, int len);    /* a record as if written before */
extern int fake_log_count;
extern char fake_log_last[256];
int fake_log_has(const char *text);     /* a log line since fake_log_clear() contained text */
void fake_log_clear(void);
#endif
