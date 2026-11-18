/* gp_power.c: see gp_power.h. */
#include "system/includes.h"
#include "asm/power/p33.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "bt_common.h"
#include "app_comm_bt.h"
#include "gamepad.h"
#include "gp_core.h"
#include "gp_bt.h"
#include "gp_leds.h"
#include "gp_rumble.h"
#include "gp_settings.h"
#include "gp_charging.h"
#include "gp_power.h"
#include "gp_log.h"

#define OFF_DISCONNECT_MS   1500            /* give up on a clean disconnect */
#define OFF_SETTLE_MS       1000            /* after the link is down, before the soft-off */

static u8 gp_powering_off;
static volatile u8 gp_active;               /* set by the tick: sticks or triggers off rest */
static volatile u8 gp_idle_restart;         /* pairing mode started: restart the off count */
static u8 gp_off_on_release;                /* Home held long enough: off when it is let go */

/* The soft-off itself. Wake-up pending flags latched while the pad was on
 * (Home's release bounces on its wake pin) made the chip wake straight back
 * up: P3_WKUP_PND read 0xFF at that boot, 0x02 (Home) after a normal wake.
 * Clear them right before going off. With 5 V present (USB or the dock):
 * charging mode. */
void gp_power_soft_off(void)
{
    if (gp_board_charge_power()) {                       /* USB or the dock */
        gp_charging_start();
        return;
    }
    p33_tx_1byte(P3_WKUP_CPND, 0xff);
    power_set_soft_poweroff();
}

/* The SDK's way down: BT stack off, then the soft-off once the link is gone. */
static void gp_power_shutdown(void)
{
    gp_log("power: soft-off");
    gp_keep_awake = 1;
    gp_bt_shutdown();
    sys_timeout_add(NULL, (void (*)(void *))gp_power_soft_off, WAIT_DISCONN_TIME_MS);
}

void gp_power_off(const char *why)
{
    if (gp_powering_off) {
        return;
    }
    gp_powering_off = 1;
    gp_log("power off: %s", why);
    gp_core_tick_stop();                                 /* no more reports, IMU or UART work */
    gp_rumble_stop();
    gp_settings_flush();
    gp_leds_dark();
    gp_board_power_down();                               /* LED and stick chip power off */
    /* Close the link first (stock: "sleep disconnect bt"). The SDK's soft-off
     * only stops scanning: the host kept the old link until its supervision
     * timeout, still showed the pad as connected, and a wake within that
     * time left the pad reconnecting with no input. gp_power_off_step
     * finishes once the link is down (or after OFF_DISCONNECT_MS).
     * The disconnect goes first and alone: queued right behind the two
     * scan-off commands it was dropped (the host kept the link until its
     * timeout). gp_power_off_step re-sends it until the link is down; the
     * HCI handler turns scanning off when the disconnect completes, and
     * keeps it off: the host pages straight back after the disconnect
     * (btmon: Create Connection 80 ms later), and the SDK's disconnect
     * handler would make the pad connectable again. */
    if (gp_bt_link_up()) {
        gp_bt_disconnect();
    } else {
        gp_bt_scan_off();
    }
}

void gp_power_off_lowbat(void)
{
    gp_power_off("low battery");
}

int gp_power_off_pending(void)
{
    return gp_powering_off;
}

void gp_power_off_step(void)
{
    static u16 ms, down_ms;
    static u8 done;
    if (done) {
        return;
    }
    ms += GP_POLL_MS;
    u8 link = gp_bt_link_up();
    if (link && ms % 200 == 0 && ms < OFF_DISCONNECT_MS) {
        gp_bt_disconnect();                              /* again, in case it was dropped */
    }
    if (!link || ms >= OFF_DISCONNECT_MS) {
        /* A second after the link is down before going off (the 0xFF
         * wake-ups came from going off with the link still up). */
        if ((down_ms += GP_POLL_MS) >= OFF_SETTLE_MS) {
            done = 1;
            gp_bt_scan_off();
            gp_power_shutdown();
        }
    }
}

void gp_power_activity(void)
{
    gp_active = 1;
}

void gp_power_restart_idle(void)
{
    gp_idle_restart = 1;
}

int gp_power_home_held(void)
{
    return gp_off_on_release;
}

void gp_power_poll(u8 connected, u8 wired)
{
    static u32 idle_ms, home_ms;
    static u8 was_connected;
    u32 buttons = gp_board_buttons();
    int pairing = gp_bt_pairing();
    if (connected != was_connected || gp_idle_restart) {
        idle_ms = 0;                                     /* each state starts its own count */
        was_connected = connected;
        gp_idle_restart = 0;
    }
    if (buttons || gp_active || wired) {
        gp_active = 0;
        if (connected) {
            idle_ms = 0;                                 /* only a connected pad stays on for input */
        }
    }
    idle_ms += GP_POLL_MS;
    if (connected && gp_settings.idle_off_min && idle_ms >= gp_settings.idle_off_min * 60000u) {
        gp_power_off("idle");
    } else if (!connected && pairing && idle_ms >= GP_PAIR_OFF_S * 1000u) {
        gp_power_off("pairing timed out");
    } else if (!connected && !pairing && idle_ms >= GP_RECONNECT_OFF_S * 1000u) {
        gp_power_off("no host");
    }
    /* Home held: the LEDs go dark at GP_HOME_OFF_MS, and the pad turns off
     * when Home is let go. Turning off with Home still down woke it straight
     * back up (the wake-up pin saw the press). */
    int home = (buttons & GP_BIT(GP_BTN_GUIDE)) != 0;
    if (buttons == GP_BIT(GP_BTN_GUIDE)) {               /* Home alone: not a combo (pairing, modes) */
        home_ms += GP_POLL_MS;
        if (home_ms >= GP_HOME_OFF_MS && !gp_off_on_release) {
            gp_off_on_release = 1;
            gp_leds_dark();
        }
    } else {
        home_ms = 0;
        if (gp_off_on_release && !home) {
            static u16 released_ms;
            if ((released_ms += GP_POLL_MS) >= 300) {    /* let Home's release bounce settle */
                gp_power_off("Home held");
            }
        }
    }
}
