/* dg_main.c: the C2 receiver's application (docs/dongle.md), in apps/hid's
 * framework as the pad's gp_main.c: start-up, the button (pairing), the LED,
 * the crash-loop guard's "stable" mark, the µs clock and the debug-log
 * commands. USB: dg_usb.c; Bluetooth: dg_bt.c. */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "app_action.h"
#include "os/os_api.h"

#ifdef CONFIG_DONGLE_FW

#include <string.h>
#include "dongle.h"
#include "gp_log.h"

extern unsigned long jiffies_msec(void);

/* Symbols the shared SDK patches expect from the firmware (patch_sdk.py). */
volatile u32 gp_uart2_slips, gp_uart2_bogus;
volatile int gp_hid_send_err;
volatile u32 gp_hid_send_errs;

/* ---- A microsecond clock ----------------------------------------------------
 * TIMER1 is the SDK's usr_timer base: it counts the 24 MHz clock up to PRD
 * (2 ms) and interrupts; a usr_timer of 2 ms runs on each interrupt and
 * counts them. Between them CNT gives the fraction. If the timer wrapped
 * and its interrupt hasn't run yet (pending, BIT(15)), one more period. */
static volatile u32 dg_t1_periods;

static void dg_us_tick(void *priv)
{
    dg_t1_periods++;
}

u32 dg_us(void)
{
    u32 p, c, prd;
    OS_ENTER_CRITICAL();
    p = dg_t1_periods;
    c = JL_TIMER1->CNT;
    prd = JL_TIMER1->PRD;
    if (JL_TIMER1->CON & BIT(15)) {
        c = JL_TIMER1->CNT;                  /* wrapped: read again after the wrap */
        p++;
    }
    OS_EXIT_CRITICAL();
    return p * 2000 + (prd ? c * 2000 / prd : 0);
}

static void dg_stable(void *priv)
{
    dg_guard_stable();
    gp_log("guard: %u s up, crash-loop count cleared", DG_STABLE_MS / 1000);
}

/* The button (PA00, low = pressed): a short press pairs. (Held at power-up
 * it is the bootloader: board_dongle.c.) */
static void dg_button_poll(void *priv)
{
    static u16 held_ms;
    if (dg_button_pressed()) {
        held_ms += DG_BUTTON_POLL_MS;
    } else {
        if (held_ms >= 50 && held_ms <= 2000) {
            gp_log("button: short press -> pairing");
            dg_bt_pair_start();
        }
        held_ms = 0;
    }
}

/* Log commands beyond core/gp_log.c's "boot" and "reset". */
int gp_log_board_command(const char *cmd)
{
    if (dg_bt_command(cmd)) {
        return 1;
    }
    if (!strcmp(cmd, "usb")) {
        dg_usb_dump();
        return 1;
    }
    if (!strcmp(cmd, "clock")) {             /* the us clock against the ms one */
        gp_log("cmd: clock: us %u, ms %u, TIMER1 CON %04x PRD %u CNT %u", (unsigned)dg_us(),
               (unsigned)jiffies_msec(), (unsigned)JL_TIMER1->CON, (unsigned)JL_TIMER1->PRD,
               (unsigned)JL_TIMER1->CNT);
        return 1;
    }
    if (!strcmp(cmd, "crash")) {             /* test the crash-loop guard: the watchdog resets */
        gp_log("cmd: hanging with interrupts off (watchdog reset)");
        local_irq_disable();
        while (1);
    }
    return 0;
}

/* The LED (PA09): slow blink idle (1 s), solid connected, fast blink
 * (100 ms) pairing. Idle for DG_LED_IDLE_MS (no pad since start-up or the
 * last disconnect): off, though it keeps reconnecting; a pad or pairing
 * lights it again. */
#define DG_LED_IDLE_MS      30000

static void dg_heartbeat(void *priv)
{
    static u8 on, ticks;
    static u16 idle_ticks;
    u8 st = dg_bt_state();
    ticks++;
    idle_ticks = st ? 0 : idle_ticks < DG_LED_IDLE_MS / 100 ? idle_ticks + 1 : idle_ticks;
    if (st == 2) {
        on = !on;
    } else if (st == 1) {
        on = 1;
    } else if (idle_ticks >= DG_LED_IDLE_MS / 100) {
        on = 0;                              /* no pad for a while: dark */
    } else if (ticks >= 10) {
        ticks = 0;
        on = !on;
    }
    dg_led(on);
    dg_usb_poll();                           /* the pad's mode -> the USB device (dg_usb.c) */
}

static void dg_app_start(void)
{
    dg_checkpoint(6);
    usr_timer_add(NULL, dg_us_tick, 2, 1);
    sys_timer_add(NULL, dg_heartbeat, 100);
    gp_log_init();
    gp_log("boot: C2 receiver (rst src %02x, boot %u of the crash-loop guard's %u)",
           dg_boot_rst_src, dg_boot_count, 3);
    dg_usb_load_mode();                      /* before the host enumerates us */
    dg_button_init();
    sys_timer_add(NULL, dg_button_poll, DG_BUTTON_POLL_MS);
    sys_timeout_add(NULL, dg_stable, DG_STABLE_MS);
    dg_bt_init();
}

static int dg_state_machine(struct application *app, enum app_state state, struct intent *it)
{
    if (state == APP_STA_START && it && it->action == ACTION_HID_MAIN) {
        dg_app_start();
    }
    return 0;
}

static int dg_event_handler(struct application *app, struct sys_event *event)
{
    if (dg_bt_event(event)) {
        return 0;
    }
    if (event->type == SYS_DEVICE_EVENT) {
        dg_usb_event(event);                 /* USB host on the cable / gone */
    }
    return 0;                                /* power events (battery) ignored: no battery */
}

static u8 dg_idle_query(void)
{
    return 0;                                /* never low-power sleep */
}

REGISTER_LP_TARGET(dg_lp_target) = {
    .name = "dongle",
    .is_idle = dg_idle_query,
};

static const struct application_operation dg_app_ops = {
    .state_machine  = dg_state_machine,
    .event_handler  = dg_event_handler,
};

REGISTER_APPLICATION(app_dongle) = {
    .name   = "hid_key",                     /* apps/hid/app_main.c starts the app by this name */
    .action = ACTION_HID_MAIN,
    .ops    = &dg_app_ops,
    .state  = APP_STA_DESTROY,
};

#endif
