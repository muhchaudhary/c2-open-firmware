/* hjc_stick.c: see hjc_stick.h. (C2 stock: opened at 0x1E3771C, frame
 * callback 0x1E6E406; boards/c2/c2_pins.h.) */
#include "system/includes.h"
#include "asm/uart_dev.h"
#include "gamepad.h"
#include "gp_log.h"
#include "hjc_stick.h"

static const u8 hjc_ids[4] = { 0xaa, 0x55, 0xcc, 0x33 };   /* LX LY RX RY */
static volatile u16 hjc_raw[4];
static volatile u8 hjc_seen;            /* bit i: axis i has had a real value */
volatile u32 hjc_stick_frames;          /* good frames received */
static volatile u32 hjc_total;          /* good frames since the last baud change */
volatile u32 hjc_stick_bad;
static volatile u32 hjc_bytes;          /* bytes received at all */
volatile u32 hjc_stick_glitches;
static const uart_bus_t *hjc_bus;
static u32 hjc_byte_n;                  /* bytes through hjc_byte (the probe's clock within a burst) */

/* The mean of every value since the last read (hjc_stick_value): the chip
 * sends ~3000 a second per axis, a report takes one every ~4 ms. */
static volatile u32 hjc_sum[4];
static volatile u16 hjc_cnt[4];
static u16 hjc_mean[4];
static u8 hjc_use_mean = 1;

/* The probe (hjc_stick.h): per axis, the first value past the threshold. */
static struct {
    u8 on, out, got;
    s8 dir;                             /* the crossing's side: +1 / -1 */
    u16 centre, thresh;
    u32 at_byte;                        /* hjc_byte_n of that frame's last byte */
    u32 t_us;
} hjc_probe[4];
/* patch_sdk.py, UART2 RX interrupt: interrupts that found more than one
 * step of bytes (the driver used to lose those), and HRXCNT values it
 * didn't trust */
volatile u32 gp_uart2_slips, gp_uart2_bogus;

/* Stock asks its driver for 941176, but its driver turns that into a clock
 * divisor ((UART_CLK + baud/2) / baud / 4 - 1), so the rate on the wire
 * depends on stock's UART clock (941176 at 64 MHz, 1 M at 48 or 24 MHz...).
 * So: try these in turn, 0.5 s each, stop at the first that gives good
 * frames. Measured: frames always lock at 500000 on the C2 (470588, stock's
 * 941176 / 2, never did), so 500000 goes first: no half-second lost at boot. */
static const u32 hjc_bauds[] = { 500000, 470588, 941176, 1000000, 921600, 960000, 1500000, 2000000, 460800, 230400, 115200 };
static u8 hjc_baud_idx;

/* The driver's DMA ring (a power of 2) and its interrupt step. The DMA
 * writes the ring round and round by itself; the driver only learns about
 * new bytes from the "frame_length bytes received" interrupt. Was 64 B /
 * every 4 B: 12500 interrupts a second at 500000 baud, and an interrupt held
 * off for over 1.3 ms (BT, critical sections) let the DMA overwrite unread
 * bytes, or two steps went by for one interrupt: 10-43 % CRC errors that came
 * and went with BT load (debug log, 2026-11-13). Now an interrupt every
 * 128 B (2.6 ms, ~390 a second) into 16x that, ~40 ms of data. */
#define HJC_RX_STEP         128
static u8 hjc_rx_buf[2048] __attribute__((aligned(4)));

static u8 crc8(const u8 *p, int n)
{
    u8 c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c & 0x80) ? (u8)((c << 1) ^ 0x31) : (u8)(c << 1);
        }
    }
    return c;
}

static int absdiff(int a, int b)
{
    return a > b ? a - b : b - a;
}

/* A value that jumps more than this from the last accepted one is held
 * back until a second frame agrees with it (within HJC_AGREE): a corrupt
 * frame that passes the CRC by chance (1 in 256) is a one-off, a real fast
 * flick repeats in the next frame (~1 ms later). */
#define HJC_JUMP            4096
#define HJC_AGREE           1024

static void hjc_probe_value(int i, u16 v)
{
    typeof(hjc_probe[0]) *p = &hjc_probe[i];
    if (!p->on) {
        return;
    }
    int d = absdiff(v, p->centre);
    if (!p->out && d >= p->thresh) {
        p->out = 1;
        p->got = 1;                                      /* time it at the end of this interrupt */
        p->dir = v > p->centre ? 1 : -1;
        p->at_byte = hjc_byte_n;
    } else if (p->out && d < p->thresh / 2) {
        p->out = 0;                                      /* back: armed for the next one */
    }
}

static void hjc_value(int i, u16 v)
{
    static u16 cand[4];
    static u8 cand_ok;
    if (!(hjc_seen & BIT(i)) || absdiff(v, hjc_raw[i]) <= HJC_JUMP ||
        ((cand_ok & BIT(i)) && absdiff(v, cand[i]) <= HJC_AGREE)) {
        hjc_raw[i] = v;
        hjc_seen |= BIT(i);
        cand_ok &= ~BIT(i);
        hjc_sum[i] += v;
        hjc_cnt[i]++;
        hjc_probe_value(i, v);
        return;
    }
    if (cand_ok & BIT(i)) {
        hjc_stick_glitches++;                            /* the previous candidate never got confirmed */
    }
    cand[i] = v;
    cand_ok |= BIT(i);
}

/* Frames are found with a sliding 4-byte window: a frame is an ID byte, two
 * value bytes and a matching CRC. After a good frame the next 4 bytes must
 * be one too; if not, that's counted bad and the window slides byte by byte
 * until it finds a frame again (value bytes can look like IDs, so the old
 * "an ID byte starts a frame" lost sync for several frames per error). */
static void hjc_byte(u8 b)
{
    static u8 w[4], n, synced;
    hjc_byte_n++;
    w[0] = w[1];
    w[1] = w[2];
    w[2] = w[3];
    w[3] = b;
    if (++n < 4) {
        return;
    }
    int axis = -1;
    for (int i = 0; i < 4; i++) {
        if (w[0] == hjc_ids[i]) {
            axis = i;
        }
    }
    if (axis < 0 || crc8(w, 3) != w[3]) {
        if (synced) {
            hjc_stick_bad++;                             /* where a frame was due */
            synced = 0;
        }
        n = 3;                                           /* slide by one byte */
        return;
    }
    n = 0;
    synced = 1;
    u16 v = (w[1] << 8) | w[2];
    /* Stock stores a value with bit 15 set as 0; here it keeps the last one:
     * a 0 dragged the learned centre (2026-11-12: LX rested at +2700). */
    if (!(v & 0x8000)) {
        hjc_value(axis, v);
    }
    hjc_stick_frames++;
    hjc_total++;
}

static void hjc_uart_isr(void *ut_bus, u32 status)
{
    const uart_bus_t *bus = ut_bus;
    u8 tmp[16];
    if (status != UT_RX && status != UT_RX_OT) {
        return;
    }
    u32 len;
    while ((len = bus->read(tmp, sizeof(tmp), 0)) > 0) {
        hjc_bytes += len;
        for (u32 i = 0; i < len; i++) {
            hjc_byte(tmp[i]);
        }
    }
    /* A probe crossing in this burst: its frame came in (bytes after it)
     * byte times before now (10 bits a byte). */
    for (int i = 0; i < 4; i++) {
        if (hjc_probe[i].got == 1) {
            u32 after = hjc_byte_n - hjc_probe[i].at_byte;
            hjc_probe[i].t_us = gp_us() - after * 10000000u / hjc_bauds[hjc_baud_idx];
            hjc_probe[i].got = 2;
        }
    }
}

void hjc_probe_arm(int axis, u16 centre, u16 thresh)
{
    hjc_probe[axis].on = 0;
    hjc_probe[axis].centre = centre;
    hjc_probe[axis].thresh = thresh;
    hjc_probe[axis].out = 0;
    hjc_probe[axis].got = 0;
    hjc_probe[axis].on = 1;
}

void hjc_probe_off(void)
{
    for (int i = 0; i < 4; i++) {
        hjc_probe[i].on = 0;
    }
}

int hjc_probe_take(int axis, u32 *t_us, int *dir)
{
    if (hjc_probe[axis].got != 2) {
        return 0;
    }
    *t_us = hjc_probe[axis].t_us;
    *dir = hjc_probe[axis].dir;
    hjc_probe[axis].got = 0;
    return 1;
}

int hjc_stick_init(u8 rx_pin)
{
    static struct uart_platform_data_t cfg = {
        .tx_pin = (u8)-1,                                /* receive only: the chip streams by itself */
        .rx_cbuf = hjc_rx_buf,
        .rx_cbuf_size = sizeof(hjc_rx_buf),
        .frame_length = HJC_RX_STEP,
        .rx_timeout = 1,
        .isr_cbfun = hjc_uart_isr,
    };
    cfg.rx_pin = rx_pin;
    cfg.baud = hjc_bauds[0];
    hjc_bus = uart_dev_open(&cfg);
    if (!hjc_bus) {
        gp_log("ERROR stick uart open failed");
        return -1;
    }
    return 0;
}

int hjc_stick_value(int axis, u16 *v)
{
    if (hjc_total < 64 || !(hjc_seen & BIT(axis))) {    /* no stick data yet (or a new baud) */
        return 0;
    }
    OS_ENTER_CRITICAL();                                 /* the UART interrupt adds to these */
    u32 sum = hjc_sum[axis];
    u16 cnt = hjc_cnt[axis];
    hjc_sum[axis] = 0;                                   /* emptied either way: they never overflow */
    hjc_cnt[axis] = 0;
    OS_EXIT_CRITICAL();
    if (!hjc_use_mean) {
        *v = hjc_raw[axis];                              /* the latest only */
        return 1;
    }
    if (cnt) {
        hjc_mean[axis] = (sum + cnt / 2) / cnt;
    } else if (!hjc_mean[axis]) {
        hjc_mean[axis] = hjc_raw[axis];                  /* nothing new yet, and no mean before */
    }
    *v = hjc_mean[axis];
    return 1;
}

void hjc_stick_mean(int on)
{
    hjc_use_mean = on;
}

int hjc_stick_baud_check(void)
{
    static u32 last_frames, last_bytes, first_t0, quiet_t0;
    static u8 first, quiet;
    if (!hjc_bus) {
        return 0;
    }
    if (hjc_stick_frames != last_frames) {
        last_frames = hjc_stick_frames;
        quiet = 0;
        return 0;
    }
    /* Only bytes without good frames count against a rate: while the line
     * is silent (the stick chip still starting after power-up) stay. 500000
     * was skipped at boot that way and the search settled on 470588, which
     * locks too but with more CRC errors. */
    if (hjc_bytes == last_bytes) {
        return 0;
    }
    last_bytes = hjc_bytes;
    /* The first moments after power-up are junk at the right rate too (after
     * a reset: bytes at 500000, no good frames for ~0.4 s, then the search
     * moved on to 470588): judge a rate only 1.5 s after the first byte. */
    u32 now = gp_ms();
    if (!first) {
        first = 1;
        first_t0 = now;
    }
    if (now - first_t0 < 1500) {
        return 0;
    }
    if (!quiet) {
        quiet = 1;                                       /* bytes, but no good frame since now */
        quiet_t0 = now;
    }
    if (now - quiet_t0 < 500) {
        return 0;
    }
    quiet = 0;
    hjc_baud_idx = (hjc_baud_idx + 1) % ARRAY_SIZE(hjc_bauds);
    hjc_bus->set_baud(hjc_bauds[hjc_baud_idx]);
    gp_log("sticks: no good frame for 0.5 s, baud -> %u", (unsigned)hjc_bauds[hjc_baud_idx]);
    hjc_total = 0;
    hjc_seen = 0;
    return 1;
}

/* The UART's divisor is computed from clk_get("uart") when it is set, and
 * the hardware keeps it when that clock changes later. The UART was opened
 * before the clock went up to 48 MHz, so "500000" ran at ~1 Mbaud (the bytes
 * came in with every bit doubled) until the baud search set a rate again:
 * sticks dead for seconds after a connect, and the search settling on
 * "470588" = 480000 on the wire, one 48 MHz step slow (15-25 % CRC errors).
 * So: re-set the rate whenever that clock changes. */
void hjc_stick_clock_check(void)
{
    static u32 clk;
    u32 now = clk_get("uart");
    if (hjc_bus && now != clk) {
        clk = now;
        hjc_bus->set_baud(hjc_bauds[hjc_baud_idx]);
    }
}
