/*
 * gp_main.c: the firmware as an application of JieLi's SDK (apps/hid's
 * application framework, registered in place of its keyboard example):
 * boot, the BT stack's start-up, and the app task's event dispatch. The
 * gamepad itself lives in core/ (gp_core.c), the hardware in boards/<name>/.
 */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "app_action.h"
#include "os/os_api.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "bt_common.h"
#include "app_power_manage.h"
#include "app_comm_bt.h"

#ifdef CONFIG_GAMEPAD_FW

#include "gamepad.h"
#include "gp_core.h"
#include "gp_bt.h"
#include "gp_usb.h"
#include "gp_power.h"
#include "gp_charging.h"
#include "gp_log.h"
#include "gp_settings.h"

extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);

volatile u8 gp_keep_awake;              /* 1: the SDK must not enter low-power sleep */
static volatile u8 gp_bt_ready;         /* BT start-up done (boot check) */

/* Boot check: the BT stack reports BT_STATUS_INIT_OK within ~0.6 s. A wake
 * from a power-off with USB in hung inside BT start-up once (2026-11-13: USB
 * up, no log, no watchdog reset; needed the USB key). Not ready after 6 s:
 * log it and restart. */
static void gp_boot_restart(void *priv)
{
    cpu_reset();
}

static void gp_boot_check(void *priv)
{
    if (!gp_bt_ready) {
        gp_log("boot: BT start-up not done after 6 s, restarting");
        sys_timeout_add(NULL, gp_boot_restart, 300);     /* let the log go out first */
    }
}

static void gp_app_start(void)
{
    gp_log_init();                                       /* first, so a hang in BT start-up shows */
    gp_settings_load();                                  /* before USB comes up: it picks the protocol */
    /* Woken from off by USB 5 V alone (Home wakes it as usual, cable or
     * not): no Bluetooth, charging mode. */
    if (gp_board_woken_by_usb_only()) {
        gp_log("boot: woken by USB 5 V only: charging mode");
        gp_board_power_down();
        gp_charging_start();
        return;
    }
    gp_log("boot: app start (%s)", gp_board_info.name);
    sys_timeout_add(NULL, gp_boot_check, 6000);
    clk_set("sys", BT_NORMAL_HZ);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    gp_bt_init();
}

static int gp_state_machine(struct application *app, enum app_state state, struct intent *it)
{
    if (state == APP_STA_START && it && it->action == ACTION_HID_MAIN) {
        gp_app_start();
    }
    return 0;
}

static int gp_event_handler(struct application *app, struct sys_event *event)
{
    bt_comm_edr_sniff_clean();
    if (event->type == SYS_BT_EVENT && (u32)event->arg == SYS_BT_EVENT_TYPE_CON_STATUS &&
        event->u.bt.event == BT_STATUS_INIT_OK) {
        gp_log("bt: init ok");
        gp_bt_after_init();
        gp_bt_ready = 1;
        gp_core_start();
        return 0;
    }
    if (gp_bt_event(event)) {
        return 0;
    }
    if (event->type == SYS_DEVICE_EVENT) {
        if (gp_usb_event(event)) {                       /* USB host on the cable / gone */
            return 0;
        }
        if ((u32)event->arg == DEVICE_EVENT_FROM_POWER) {
            return app_power_event_handler(&event->u.dev, gp_power_off_lowbat);
        }
    }
    return 0;
}

/* The SDK asks this before low-power sleep. */
static u8 gp_idle_query(void)
{
    return !gp_keep_awake;
}

REGISTER_LP_TARGET(gp_lp_target) = {
    .name = "gamepad",
    .is_idle = gp_idle_query,
};

static const struct application_operation gp_app_ops = {
    .state_machine  = gp_state_machine,
    .event_handler  = gp_event_handler,
};

REGISTER_APPLICATION(app_gamepad) = {
    .name   = "hid_key",                /* apps/hid/app_main.c starts the app by this name */
    .action = ACTION_HID_MAIN,
    .ops    = &gp_app_ops,
    .state  = APP_STA_DESTROY,
};

#endif
