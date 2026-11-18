/* fake.c: what the firmware files under test take from the SDK and the
 * board, for the host tests. */
#include <stdarg.h>
#include "test.h"
#include "app_main.h"

int t_fail, t_checks;

int t_done(const char *suite)
{
    printf("%s: %d checks, %d failed\n", suite, t_checks, t_fail);
    return t_fail ? 1 : 0;
}

/* ---- clock */
u32 fake_ms = 1000, fake_us_in_ms;
unsigned long jiffies_msec(void)
{
    return fake_ms;
}
u32 local_timer_us_time(void)
{
    return fake_us_in_ms;
}

/* ---- ADC */
u32 fake_adc_value[16], fake_adc_mv[16];
u32 adc_get_value(u32 ch)
{
    return fake_adc_value[ch & 15];
}
u32 adc_get_voltage(u32 ch)
{
    return fake_adc_mv[ch & 15];
}
void adc_add_sample_ch(u32 ch)
{
    (void)ch;
}
struct app_var_t app_var;

/* ---- store: records in memory */
#define FAKE_IDS 64
static u8 store[FAKE_IDS][512];
static int store_len[FAKE_IDS];
int fake_store_fail;

void fake_store_clear(void)
{
    memset(store_len, 0, sizeof(store_len));
    fake_store_fail = 0;
}
int fake_store_put(u8 id, const void *buf, int len)
{
    memcpy(store[id], buf, len);
    store_len[id] = len;
    return len;
}
int gp_store_read(u8 id, void *buf, int len)
{
    if (id >= FAKE_IDS || !store_len[id]) {
        return -1;
    }
    int n = MIN(len, store_len[id]);
    memcpy(buf, store[id], n);
    return n;
}
int gp_store_write(u8 id, const void *buf, int len)
{
    if (fake_store_fail || id >= FAKE_IDS || len > (int)sizeof(store[0])) {
        return -1;
    }
    return fake_store_put(id, buf, len);
}

/* ---- log: kept, printed with TEST_VERBOSE=1 */
int fake_log_count;
char fake_log_last[256];
static char log_all[8192];

void fake_log_clear(void)
{
    log_all[0] = 0;
    fake_log_count = 0;
}
int fake_log_has(const char *text)
{
    return strstr(log_all, text) != NULL;
}
void gp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fake_log_last, sizeof(fake_log_last), fmt, ap);
    va_end(ap);
    fake_log_count++;
    size_t n = strlen(log_all);
    if (n + strlen(fake_log_last) + 2 < sizeof(log_all)) {
        strcat(log_all, fake_log_last);
        strcat(log_all, "\n");
    }
    if (getenv("TEST_VERBOSE")) {
        printf("      log: %s\n", fake_log_last);
    }
}
