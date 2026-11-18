/* gp_config.c: see gp_config.h and docs/config-protocol.md. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_core.h"
#include "gp_sinput.h"
#include "gp_settings.h"
#include "gp_usb.h"
#include "gp_config.h"
#include "gp_cal.h"
#include "gp_log.h"
#include "gp_leds.h"
#if __has_include("gp_version.h")
#include "gp_version.h"                 /* written by firmware/build.sh */
#endif
#ifndef GP_FW_VERSION
#define GP_FW_VERSION       "unknown"
#endif
#ifndef GP_FW_MAJOR                     /* firmware/VERSION, via build.sh */
#define GP_FW_MAJOR         0
#define GP_FW_MINOR         0
#define GP_FW_PATCH         0
#endif

#define CFG_REQ_LEN         47          /* output report 0x03 after its ID */
#define CFG_NAME_LEN        24

/* ---- The settings: gp_settings.h's table. Values travel as u16 so wider
 * ones can come later without a new layout; per-profile ones are the
 * active profile's. */
static u8 *cfg_val(const struct gp_setting_desc *d)
{
    return gp_settings_ptr(d->key, gp_settings.profile);
}

/* ---- Request / reply slots -------------------------------------------------
 * One request at a time: the client waits for the reply (or its timeout)
 * before the next one; a request arriving while one waits replaces it. */
static u8 cfg_req[CFG_REQ_LEN];
static volatile u8 cfg_req_len;         /* 0: none waiting */
static volatile u8 cfg_req_serial;      /* it came over the log port: the reply goes there */
static u8 cfg_rep[SINPUT_REPORT_LEN];
static volatile u8 cfg_rep_ready;       /* the tick sends cfg_rep */
static u8 cfg_after;                    /* what to do once the reply is out: 1 restart, 2 bootloader */
static u8 *cfg_restart_ptr;             /* a restart setting's new value, applied just before the restart */
static u16 cfg_restart_val;
static u16 cfg_after_ms;

static void cfg_take(const u8 *c, int size, u8 serial)
{
    if (size < 3) {
        return;                                          /* no seq / op: nothing to answer */
    }
    OS_ENTER_CRITICAL();
    cfg_req_serial = serial;
    memcpy(cfg_req, c, MIN(size, CFG_REQ_LEN));
    if (size < CFG_REQ_LEN) {
        memset(cfg_req + size, 0, CFG_REQ_LEN - size);
    }
    cfg_req_len = MIN(size, CFG_REQ_LEN);
    OS_EXIT_CRITICAL();
}

void gp_config_request(const u8 *c, int size)
{
    cfg_take(c, size, 0);
}

/* "cfg <hex>" on the log port (gp_log.c): any mode, wired. */
void gp_log_cfg_request(const u8 *req, int len)
{
    cfg_take(req, len, 1);
}

int gp_config_reply(u8 *rep)
{
    if (!cfg_rep_ready) {
        return 0;
    }
    memcpy(rep, cfg_rep, SINPUT_REPORT_LEN);
    cfg_rep_ready = 0;
    return 1;
}

void gp_config_retry(void)
{
    cfg_rep_ready = 1;                                   /* cfg_rep is unchanged until the next request is answered */
}

static void put16(u8 *p, u16 v)
{
    p[0] = v;
    p[1] = v >> 8;
}

static u16 get16(const u8 *p)
{
    return p[0] | (p[1] << 8);
}

static void put_str(u8 *p, const char *s, int n)
{
    memset(p, 0, n);
    strncpy((char *)p, s, n - 1);
}

/* Answer the waiting request into cfg_rep. Returns the reply status. */
static u8 cfg_answer(const u8 *q, int len, u8 *out)
{
    const u8 *a = q + 3;                                 /* the request's payload */
    int alen = len - 3;
    switch (q[2]) {
    case GP_CFG_OP_INFO: {
        struct gp_battery b;
        gp_board_battery(&b);
        out[0] = GP_CFG_PROTO;
        put_str(out + 1, GP_FW_VERSION, 16);             /* [1..16] firmware: git commit */
        put_str(out + 17, gp_board_info.name, 16);       /* [17..32] board / Bluetooth name */
        out[33] = gp_settings.mode;
        out[34] = gp_usb_host_ready() ? 1 : 0;           /* 0 Bluetooth, 1 USB */
        put16(out + 35, b.mv);
        out[37] = b.percent;
        out[38] = b.plug;                                /* SInput: 2 charging, 3 full, 4 on battery */
        out[39] = gp_settings_count();
        out[40] = gp_settings.profile;
        out[41] = (u8)gp_board_charger_pin();            /* 0 charging, 1 done / no 5 V, 0xFF: none */
        out[42] = (gp_board_usb_power() ? 1 : 0) | (gp_board_docked() == 1 ? 2 : 0);
        out[43] = GP_FW_MAJOR;                           /* [43..45] release (protocol 3) */
        out[44] = GP_FW_MINOR;
        out[45] = GP_FW_PATCH;
        return GP_CFG_OK;
    }
    case GP_CFG_OP_LIST: {
        struct gp_setting_desc d;
        if (alen < 1) {
            return GP_CFG_ERR_LEN;
        }
        if (!gp_settings_desc(a[0], &d)) {
            return GP_CFG_ERR_KEY;
        }
        out[0] = d.key;
        out[1] = d.flags;
        put16(out + 2, d.min);
        put16(out + 4, d.max);
        put16(out + 6, d.def);
        put16(out + 8, *cfg_val(&d));
        put_str(out + 10, d.name, CFG_NAME_LEN);
        return GP_CFG_OK;
    }
    case GP_CFG_OP_GET:
    case GP_CFG_OP_SET: {
        struct gp_setting_desc d;
        if (alen < (q[2] == GP_CFG_OP_SET ? 3 : 1)) {
            return GP_CFG_ERR_LEN;
        }
        if (!gp_settings_find(a[0], &d)) {
            return GP_CFG_ERR_KEY;
        }
        u8 *val = cfg_val(&d);
        if (q[2] == GP_CFG_OP_SET) {
            u16 v = get16(a + 1);
            if (v < d.min || v > d.max) {
                return GP_CFG_ERR_VALUE;
            }
            if (d.flags & GP_SET_F_RESTART) {
                /* Not applied while running: the pad would change protocol
                 * under the host's feet (2026-11-12, mode 1 at once: Xbox
                 * reports read with the SInput descriptor, phantom paddle and
                 * triggers, the host's rumble taken as Xbox rumble, and the
                 * reply never went out, so no restart). Saved for the next
                 * boot; the restart after the reply brings it in. */
                if (*val != v) {
                    cfg_restart_val = v;
                    cfg_restart_ptr = val;
                    cfg_after = 1;
                    gp_log("config: %s = %u after a restart", d.name, v);
                }
                out[0] = d.key;
                put16(out + 1, v);
                return GP_CFG_OK;
            }
            if (*val != v) {
                *val = v;
                gp_settings_changed();
                gp_log("config: %s = %u", d.name, v);
                if (d.key == GP_CFG_KEY_PROFILE) {
                    gp_leds_show_profile();
                }
            }
        }
        out[0] = d.key;
        put16(out + 1, *val);
        return GP_CFG_OK;
    }
    case GP_CFG_OP_DEFAULTS: {
        struct gp_setting_desc d;
        gp_settings_set_defaults(0);                     /* all but the restart ones */
        for (int i = 0; gp_settings_desc(i, &d); i++) {
            u8 *val = cfg_val(&d);
            if ((d.flags & GP_SET_F_RESTART) && *val != d.def) {
                cfg_restart_val = d.def;                 /* as SET: at the restart, not now */
                cfg_restart_ptr = val;
                cfg_after = 1;
            }
        }
        gp_settings_changed();
        gp_log("config: defaults");
        return GP_CFG_OK;
    }
    case GP_CFG_OP_REBOOT:
        cfg_after = 1;
        return GP_CFG_OK;
    case GP_CFG_OP_BOOTLOADER:
        if (!gp_board_usb_power()) {
            return GP_CFG_ERR_STATE;                     /* the bootloader is USB only */
        }
        cfg_after = 2;
        return GP_CFG_OK;
    case GP_CFG_OP_CAL_START ... GP_CFG_OP_CAL_GET:
        return gp_cal_op(q[2], a, alen, out);
    default:
        return GP_CFG_ERR_OP;
    }
}

void gp_config_poll(void)
{
    if (cfg_after) {
        /* The reply goes out first (the tick sends it within a few ms; the
         * restart waits 300 ms after so a retried one and the log make it
         * too, and 1.5 s at most). */
        cfg_after_ms += GP_POLL_MS;
        if ((!cfg_rep_ready && cfg_after_ms >= 300) || cfg_after_ms >= 1500) {   /* the reply out, or given up on */
            u8 what = cfg_after;
            cfg_after = 0;
            if (cfg_restart_ptr) {
                /* Only now, with the reply out and the restart next: no more
                 * reports first (a tick in between would send the new
                 * protocol's), then the value, saved. */
                gp_core_tick_stop();
                *cfg_restart_ptr = cfg_restart_val;
                cfg_restart_ptr = NULL;
                gp_settings_changed();
            }
            gp_settings_flush();
            if (what == 2) {
                gp_log("config: entering the USB bootloader");
                gp_board_enter_bootloader();
            }
            gp_core_restart("config");
        }
        return;
    }
    if (!cfg_req_len || cfg_rep_ready) {
        return;                                          /* nothing asked, or the last reply still waiting */
    }
    u8 q[CFG_REQ_LEN];
    OS_ENTER_CRITICAL();
    int len = cfg_req_len;
    u8 serial = cfg_req_serial;
    memcpy(q, cfg_req, sizeof(q));
    cfg_req_len = 0;
    OS_EXIT_CRITICAL();
    memset(cfg_rep, 0, sizeof(cfg_rep));
    cfg_rep[0] = GP_CFG_CMD;
    cfg_rep[1] = q[1];                                   /* seq: the client matches replies by it */
    cfg_rep[2] = q[2];
    cfg_rep[3] = cfg_answer(q, len, cfg_rep + 4);
    cfg_after_ms = 0;
    if (serial) {
        gp_log_cfg_reply(cfg_rep, SINPUT_REPORT_LEN);    /* the log port: out now, not by the tick */
    } else {
        cfg_rep_ready = 1;
    }
}
