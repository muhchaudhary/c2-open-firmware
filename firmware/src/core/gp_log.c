/*
 * gp_log.c: the debug log over USB serial (see gp_log.h).
 *
 * USB: gp_usb.c brings the device up (the gamepad plus this log, or the
 * log alone in charging mode) and attaches the CDC interface here.
 *
 * cdc_write_data() sends nothing until the host has opened the port (DTR
 * set), so the ring keeps the lines until then.
 */
#include "system/includes.h"
#include "app_config.h"
#include "os/os_api.h"
#include "usb/device/usb_stack.h"
#include "usb/device/cdc.h"
#include "usb/usb_config.h"
#include "gp_log.h"
#include "gp_core.h"
#include "gamepad.h"
#include <stdarg.h>
#include <string.h>

#ifdef CONFIG_GAMEPAD_FW

#define GP_LOG_SIZE     4096            /* power of two */
#define GP_LOG_LINE     208
#define GP_LOG_FLUSH_MS 20

static u8 gp_log_ring[GP_LOG_SIZE];
static volatile u32 gp_log_head, gp_log_tail;   /* free-running byte counts */
static volatile u32 gp_log_dropped;             /* lines dropped to make room */
static volatile s8 gp_log_usb = -1;             /* USB id while the device is up */
static volatile u8 gp_log_rx;                   /* the host sent something */
volatile u8 gp_hang_count;

extern void cpu_reset(void);
extern unsigned long jiffies_msec(void);

void gp_log(const char *fmt, ...)
{
    char line[GP_LOG_LINE];
    u32 ms = jiffies_msec();
    snprintf(line, sizeof(line), "%5u.%03u ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    u32 n = strlen(line);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = 0;
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        n--;
    }
    line[n++] = '\r';
    line[n++] = '\n';

    OS_ENTER_CRITICAL();
    if (gp_log_head - gp_log_tail + n > GP_LOG_SIZE) {
        /* full: drop whole lines from the oldest end */
        while (gp_log_head - gp_log_tail + n > GP_LOG_SIZE) {
            while (gp_log_tail != gp_log_head &&
                   gp_log_ring[gp_log_tail++ & (GP_LOG_SIZE - 1)] != '\n') {
            }
        }
        gp_log_dropped++;
    }
    for (u32 i = 0; i < n; i++) {
        gp_log_ring[(gp_log_head + i) & (GP_LOG_SIZE - 1)] = line[i];
    }
    gp_log_head += n;
    OS_EXIT_CRITICAL();
}

/* ---- commands from the host ---------------------------------------------- */
/* ---- Configuration over the log port (docs/config-protocol.md, "Serial"):
 * "cfg <hex>" is a request (the bytes after report 0x03's ID, trailing
 * zeros may be left out); the reply comes back as a line "cfg> <hex>" (the
 * bytes after report 0x02's ID, trailing zeros left out). The pad answers
 * it itself (gp_config.c), the receiver passes it to the pad (dg_bt.c). */
__attribute__((weak)) void gp_log_cfg_request(const u8 *req, int len)
{
    gp_log("cmd: cfg: no configuration here");
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void gp_log_cfg_command(const char *s)
{
    u8 req[47];
    int n = 0;
    while (*s && n < (int)sizeof(req)) {
        if (*s == ' ') {
            s++;
            continue;
        }
        int hi = hexval(s[0]), lo = s[1] ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0) {
            gp_log("cmd: cfg: not hex");
            return;
        }
        req[n++] = hi << 4 | lo;
        s += 2;
    }
    if (n < 3) {
        gp_log("cmd: cfg: too short (C0 seq op ...)");
        return;
    }
    memset(req + n, 0, sizeof(req) - n);                 /* whole, as report 0x03: a payload's zeros count */
    gp_log_cfg_request(req, sizeof(req));
}

void gp_log_cfg_reply(const u8 *rep, int len)
{
    char hex[2 * 63 + 1];
    while (len > 4 && !rep[len - 1]) {
        len--;                                           /* trailing zeros: the client pads them */
    }
    if (len > 63) {
        len = 63;
    }
    for (int i = 0; i < len; i++) {
        snprintf(hex + 2 * i, 3, "%02x", rep[i]);
    }
    hex[2 * len] = 0;
    gp_log("cfg> %s", hex);
}

/* The pad's core (gp_core.c) has this; the receiver has no battery. */
__attribute__((weak)) void gp_core_lowbat_test(void)
{
    gp_log("cmd: no battery here");
}

/* A board's own commands (the receiver's): 1 if it took the command. */
__attribute__((weak)) int gp_log_board_command(const char *cmd)
{
    return 0;
}

static void gp_log_command(char *cmd)
{
    if (!strcmp(cmd, "boot")) {
        gp_log("cmd: entering the USB bootloader");
        gp_board_enter_bootloader();
    } else if (!strcmp(cmd, "reset")) {
        cpu_reset();
    } else if (!strncmp(cmd, "cfg ", 4)) {
        gp_log_cfg_command(cmd + 4);
    } else if (!strcmp(cmd, "lowbat test")) {
        gp_log("cmd: low-battery warning for 10 s");
        gp_core_lowbat_test();
    } else if (gp_log_board_command(cmd)) {
    } else if (cmd[0]) {
        gp_log("cmd: unknown \"%s\" (boot, reset)", cmd);
    }
}

static void gp_log_read_commands(void)
{
    static char cmd[112];                       /* "cfg " + a whole request in hex: 98 */
    static u8 len;
    u8 buf[64];
    gp_log_rx = 0;
    /* The OUT endpoint is double-buffered and one read takes one packet:
     * read until it is empty, else a packet waits for the next one to come
     * in (commands were lost, 2026-11-13). */
    for (int k = 0; k < 4; k++) {
        u32 n = cdc_read_data(gp_log_usb, buf, sizeof(buf));
        if (!n) {
            break;
        }
        for (u32 i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\r' || c == '\n') {
                cmd[len] = 0;
                len = 0;
                gp_log_command(cmd);
            } else if (len < sizeof(cmd) - 1) {
                cmd[len++] = c;
            }
        }
    }
}

/* The CDC OUT endpoint's interrupt: read it from the task. */
static void gp_log_cdc_rx(struct usb_device_t *usb_device)
{
    gp_log_rx = 1;
}

/* ---- sending ------------------------------------------------------------ */
static void gp_log_flush(void *priv)
{
    if (gp_log_usb < 0) {
        return;
    }
    static u8 rx_poll;
    if (gp_log_rx || ++rx_poll >= 10) {          /* the interrupt's flag, or every ~100 ms anyway */
        rx_poll = 0;
        gp_log_read_commands();
    }
    for (int k = 0; k < 2; k++) {                /* up to 128 B per call: the task runs above BT */
        u8 chunk[64];
        u32 tail, n;
        OS_ENTER_CRITICAL();
        tail = gp_log_tail;
        n = gp_log_head - tail;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        for (u32 i = 0; i < n; i++) {
            chunk[i] = gp_log_ring[(tail + i) & (GP_LOG_SIZE - 1)];
        }
        OS_EXIT_CRITICAL();
        if (!n) {
            /* all sent, so the port is open: say if lines were lost */
            static u32 dropped_said;
            if (gp_log_dropped != dropped_said) {
                gp_log("(log: %u older lines were dropped, the ring was full)",
                       (unsigned)(gp_log_dropped - dropped_said));
                dropped_said = gp_log_dropped;
            }
            return;
        }
        /* Only into a free endpoint. With the cable in and nothing reading
         * the port, the last packet is never collected and the SDK's write
         * waits on it: this task (priority 5, above BT) then starved the
         * rest, ~60 Bluetooth reports/s instead of ~180 (2026-11-14). */
        if (usb_read_txcsr(gp_log_usb, CDC_DATA_EP_IN) & TXCSRP_TxPktRdy) {
            return;                              /* not collected yet: keep it */
        }
        u32 sent = cdc_write_data(gp_log_usb, chunk, n);
        if (!sent) {
            return;                              /* port not open (or busy): keep it */
        }
        OS_ENTER_CRITICAL();
        if (gp_log_tail == tail) {               /* else the writer dropped lines meanwhile */
            gp_log_tail = tail + sent;
        }
        OS_EXIT_CRITICAL();
    }
}

/* Its own task, so the log still goes out while the app task (priority 1,
 * the lowest) is stuck: a hang in it took the last seconds of log with it
 * (2026-11-13, rst 04). Priority 5: above the BT tasks (3, 4), so a spinning
 * BT task can't silence it either; it sends at most 128 B per 10 ms and
 * sleeps in between, so it takes little from them. */
static void gp_log_task(void *priv)
{
    while (1) {
        gp_log_flush(NULL);
        os_time_dly(1);
    }
}

void gp_log_init(void)
{
    if (os_task_create(gp_log_task, NULL, 5, 256, 0, "gp_log")) {
        sys_timer_add(NULL, gp_log_flush, GP_LOG_FLUSH_MS);   /* no task: from the app task */
        gp_log("log: no task, flushing from the app task");
    }
}

/* gp_usb.c: the CDC interface is up on USB device usb_id (-1: gone). */
void gp_log_usb_attach(int usb_id)
{
    if (usb_id >= 0) {
        cdc_set_wakeup_handler(gp_log_cdc_rx);
    }
    gp_log_usb = usb_id;
}

#else
volatile u8 gp_hang_count;
void gp_log(const char *fmt, ...) {}
void gp_log_init(void) {}
void gp_log_usb_attach(int usb_id) {}
#endif
