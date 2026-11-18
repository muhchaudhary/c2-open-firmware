/* dg_bt.c: the receiver's Bluetooth, stage B (docs/dongle.md): does the
 * public SDK's classic-BT library, built for a HID *device*, work as the
 * pad's host? The receiver starts the stack as the pad does (gp_bt.c), is
 * connectable and discoverable, and logs every event and every packet that
 * arrives on the HID channels (user_hid_init's handler). From the debug log:
 *   bt connect AA:BB:CC:DD:EE:FF   page that address (as written by Linux)
 *   bt hid                          ask the library to open the HID channels
 *   bt pair                         search for a pad in pairing mode (as the button)
 *   bt disc                         drop the link
 *   stats / stats on / stats off    rates and latency: once / every second
 * The library pages and opens the HID channels the way a device reconnects
 * to its host; the pad, a HID device itself, accepts a host's channels and
 * sends its input reports on them. */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "btcontroller_config.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "bt_common.h"
#include "edr_hid_user.h"
#include "app_comm_bt.h"

#ifdef CONFIG_DONGLE_FW

#include <string.h>
#include "dongle.h"
#include "gp_log.h"
#include "gp_link.h"
#include "gp_sinput.h"

extern void bt_set_local_name(char *name, u8 len);
extern void bt_pll_para(u32 osc, u32 sys, u8 low_power, u8 xosc);
extern void bt_set_mac_addr(u8 *addr);
extern int hid_diy_set_sendbuf_size(u16 size);
extern unsigned long jiffies_msec(void);
extern int edr_hid_data_send_ext(u8 report_type, u8 report_id, u8 *data, u16 len);

/* The library wants a HID descriptor for its own (device) record; the
 * receiver never sends on it. A one-byte vendor input report. */
static const u8 dg_bt_report_desc[] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01,
    0x85, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x09, 0x01, 0x81, 0x02,
    0xC0,
};

static const edr_sniff_par_t dg_sniff_param = {
    .sniff_mode = SNIFF_MODE_ANCHOR,
    .cnt_time = 1,
    .max_interval_slots = 16,
    .min_interval_slots = 16,
    .attempt_slots = 2,
    .timeout_slots = 1,
    .check_timer_period = 200,
};

static edr_init_cfg_t dg_edr_config = {
    .page_timeout = 8000,
    .super_timeout = 8000,
    .io_capabilities = 3,                    /* NoInputNoOutput: just-works pairing */
    .passkey_enable = 0,
    .authentication_req = 2,
    .oob_data = 0,
    .sniff_param = &dg_sniff_param,
    .class_type = BD_CLASS_GAMEPAD,
    .report_map = dg_bt_report_desc,
    .report_map_size = sizeof(dg_bt_report_desc),
};

/* ---- What arrives on the HID channels ------------------------------------ */
static volatile u32 dg_rx_count, dg_rx_bytes;

static volatile u8 dg_hid;                  /* HID channels up (reports arriving); see below */

/* Configuration over the receiver's log port ("cfg <hex>", gp_log.c): the
 * request goes to the pad as SInput's command report 0x03, as from USB; the
 * pad's reply (report 0x02, C0 and the same seq) is printed to the log
 * instead of going to USB, so it works in Xbox mode too (XInput has no
 * place for it). */
#define DG_CFG_CMD          0xC0            /* core/gp_config.h GP_CFG_CMD */
static volatile u8 dg_cfg_wait, dg_cfg_seq;

void gp_log_cfg_request(const u8 *req, int len)
{
    if (dg_bt_state() != 1) {
        gp_log("cmd: cfg: no pad connected");
        return;
    }
    dg_cfg_seq = req[1];
    dg_cfg_wait = 1;
    dg_bt_output(SINPUT_ID_COMMAND, req, len);
}

static int dg_cfg_reply(const u8 *packet, u16 size)
{
    if (!dg_cfg_wait || size < 5 || packet[0] != 0xa1 || packet[1] != SINPUT_ID_FEATURES ||
        packet[2] != DG_CFG_CMD || packet[3] != dg_cfg_seq) {
        return 0;
    }
    dg_cfg_wait = 0;
    gp_log_cfg_reply(packet + 2, size - 2);
    return 1;
}

static void dg_bt_hid_in(u8 *packet, u16 size, u16 channel)
{
    if (size >= 2 && packet[0] == 0xa1 && (packet[1] & 0xf0) == 0x20) {
        dg_link_in(packet[1], packet + 2, size - 2);   /* the receiver link (core/gp_link.h) */
    } else if (!dg_cfg_reply(packet, size)) {
        dg_usb_forward(packet, size);        /* an SInput report (or a configuration reply) as it is */
    }
    dg_hid = 1;
    dg_rx_count++;
    dg_rx_bytes += size;
    if (dg_rx_count == 1) {                  /* the first packet (up to 24 B) */
        char hex[24 * 3 + 1];
        int n = size < 24 ? size : 24;
        for (int i = 0; i < n; i++) {
            snprintf(hex + i * 3, 4, "%02x ", packet[i]);
        }
        hex[n * 3] = 0;
        gp_log("bt rx #%u ch %04x len %u: %s", (unsigned)dg_rx_count, channel, size, hex);
    }
}

/* Once a second: the rates and latency (dongle.h) of the last second. Logged
 * on `stats` (once), `stats on` / `stats off` (every second), else once a
 * minute while reports flow. */
static u8 dg_stats_once, dg_stats_on;

static void dg_bt_rx_report(void *priv)
{
    static u32 count0, bytes0, sent0, drop0, flush0, lost0;
    static u8 secs;
    u32 c = dg_rx_count, b = dg_rx_bytes, sent = dg_usb_sent, drop = dg_usb_dropped, flush = dg_usb_flushed;
    u32 lost = dg_link_lost;
    struct dg_lat l;
    OS_ENTER_CRITICAL();
    memcpy(&l, (void *)&dg_lat, sizeof(l));
    memset((void *)&dg_lat, 0, sizeof(l));
    OS_EXIT_CRITICAL();
    int show = dg_stats_on || dg_stats_once || (c != count0 && ++secs >= 60);
    if (show) {
        secs = 0;
        dg_stats_once = 0;
        gp_log("stats: bt rx %u reports/s (%u B), usb out %u/s, replaced unsent %u, stale flushed %u, "
               "re-setups %u, link %s, lost %u",
               (unsigned)(c - count0), (unsigned)(b - bytes0), (unsigned)(sent - sent0),
               (unsigned)(drop - drop0), (unsigned)(flush - flush0), (unsigned)dg_usb_resetups,
               dg_link_active() ? "yes" : "no", (unsigned)(dg_link_lost - lost0));
        if (l.wait_n) {
            gp_log("stats: bt->usb wait avg %u max %u us", (unsigned)(l.wait_sum / l.wait_n),
                   (unsigned)l.wait_max);
        }
        if (l.echo_n) {
            gp_log("stats: echo x%u: usb->bt avg %u max %u, bt round trip avg %u max %u, ->usb avg %u max %u us",
                   (unsigned)l.echo_n, (unsigned)(l.out_sum / l.echo_n), (unsigned)l.out_max,
                   (unsigned)(l.air_sum / l.echo_n), (unsigned)l.air_max,
                   (unsigned)(l.usb_sum / l.echo_n), (unsigned)l.usb_max);
        }
    }
    count0 = c;
    bytes0 = b;
    sent0 = sent;
    drop0 = drop;
    flush0 = flush;
    lost0 = lost;
}

/* ---- Host -> pad: SInput's command report (rumble, LEDs, ...) ----------------
 * From USB (SET_REPORT, an interrupt): kept here and sent from the app task,
 * never from the interrupt (the pad reset when its tick sent over BT). On the
 * wire `a2 <id> <data>` (HIDP DATA | OUTPUT): the pad's gp_bt_hid_output
 * takes that. Newest wins, in two slots: the lightbar colour (SInput 04,
 * DS4 mode) and everything else, so a DS4 output report's rumble and
 * colour (one report, two commands) don't replace each other. */
#define DG_OUT_MAIN         0
#define DG_OUT_RGB          1
static u8 dg_out[2][64];
static volatile u8 dg_out_len[2];            /* 0: nothing waiting; else id + data */
static volatile u32 dg_out_sent, dg_out_lost;

void dg_bt_output(u8 id, const u8 *data, u16 len)
{
    if (len > sizeof(dg_out[0]) - 1) {
        len = sizeof(dg_out[0]) - 1;
    }
    int k = id == SINPUT_ID_COMMAND && len >= 1 && data[0] == 0x04 ? DG_OUT_RGB : DG_OUT_MAIN;
    OS_ENTER_CRITICAL();
    if (dg_out_len[k]) {
        dg_out_lost++;
    }
    dg_out[k][0] = id;
    memcpy(dg_out[k] + 1, data, len);
    dg_out_len[k] = len + 1;
    OS_EXIT_CRITICAL();
}

static void dg_bt_output_send(int k)
{
    if (!dg_out_len[k]) {
        return;
    }
    u8 buf[64];
    u8 n;
    OS_ENTER_CRITICAL();
    n = dg_out_len[k];
    memcpy(buf, dg_out[k], n);
    dg_out_len[k] = 0;
    OS_EXIT_CRITICAL();
    int ret = edr_hid_data_send_ext(0xa2, buf[0], buf + 1, n - 1);
    if (ret) {
        gp_log("bt: output report %02x not sent (%d)", buf[0], ret);
    } else {
        dg_out_sent++;
        if (buf[0] == GP_LINK_ID_HELLO_REQ && dg_echo_state == 1) {   /* a ping's hello request */
            dg_echo_t1 = dg_us();
            dg_echo_state = 2;
        }
    }
}

static void dg_bt_output_poll(void *priv)
{
    dg_bt_output_send(DG_OUT_MAIN);
    dg_bt_output_send(DG_OUT_RGB);
}

/* ---- Reconnection and pairing --------------------------------------------
 * Reconnection: at start-up the library pages the bonded pad
 * (bt_connect_phone_back_start: the last device in its bond list); while
 * not connected the receiver stays connectable (a pad that pages its host
 * gets in) and pages the bonded pad again every DG_RECONNECT_MS (a pad that
 * waits instead). The SDK's own handlers try once, a host's job.
 * Pairing (dg_bt_pair_start: the button, or `bt pair`): drop the link, stop
 * scanning, inquiry ~10 s; the first pad found on its receiver link (the
 * name DG_PAD_LINK_NAME, the pad's gp_bt.c: its own identity for us, so a
 * pad's normal Bluetooth identity is never taken): forget the old bond,
 * connect. Inquiry timeout: back to reconnecting. */
#define DG_PAD_LINK_NAME    "C2 pad (receiver link)"
static void dg_bt_log_addr(const char *what, const u8 *a);

/* Connectable (a pad on its receiver link pages us at its start-up) but not
 * discoverable: nothing needs to find the receiver, it searches itself
 * (it showed up as "C2 receiver" in the PC's Bluetooth list). */
static void dg_bt_connectable(void)
{
    bt_wait_phone_connect_control(1);
    user_send_cmd_prepare(USER_CTRL_WRITE_SCAN_DISABLE, 0, NULL);
}
#define DG_RECONNECT_MS     5000

/* A page that never ends. After a Connection Timeout (0x08) the SDK's
 * handler paged again at once and the library then sat "paging" with no
 * result: no page, no page scan (the pad's own page unanswered), our
 * retries ignored, until a restart (2026-11-14). Failed connections are
 * ours now (dg_bt_event), and a page with no Connection Complete within
 * DG_PAGE_STUCK_MS (page timeout 8000 slots = 5 s) is cancelled; after
 * DG_PAGE_STUCK_MAX of those in a row the receiver restarts. */
#define DG_PAGE_STUCK_MS    15000
#define DG_PAGE_STUCK_MAX   3
static volatile u32 dg_page_since;          /* jiffies_msec of the outstanding page | 1, 0: none */
static u8 dg_page_stuck;                    /* stuck pages in a row */

static void dg_bt_restart(void *priv)
{
    cpu_reset();
}

static void dg_bt_paging(void)
{
    dg_page_since = jiffies_msec() | 1;
}
#define DG_INQUIRY_LEN      8               /* x 1.28 s */
#define DG_PAIR_TIMEOUT_MS  15000           /* pairing ends by itself, whatever the library says */

extern void inquiry_result_handle_register(void (*handle)(char *name, u8 name_len, u8 *addr, u32 dev_class, char rssi));
extern int bt_connect_phone_back_start(void);

static volatile u8 dg_acl;                  /* ACL link up */
static volatile u8 dg_hid;                  /* HID channels up (reports arriving) */
static volatile u8 dg_pairing;
static volatile u8 dg_found;                /* pairing: a pad found, dg_found_addr */
static u8 dg_found_addr[6];

u8 dg_bt_state(void)                        /* 0 idle, 1 connected, 2 pairing */
{
    return dg_pairing ? 2 : (dg_acl && dg_hid) ? 1 : 0;
}

static void dg_bt_inquiry_result(char *name, u8 name_len, u8 *addr, u32 dev_class, char rssi)
{
    char n[33];
    int l = name_len < 32 ? name_len : 32;
    memcpy(n, name ? name : "", name ? l : 0);
    n[name ? l : 0] = 0;
    gp_log("bt: found %02X:%02X:%02X:%02X:%02X:%02X class %06x rssi %d \"%s\"",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], (unsigned)dev_class, rssi, n);
    /* "C2 pad (": a shortened name (EIR) still matches */
    if (dg_pairing && !dg_found && !strncmp(n, DG_PAD_LINK_NAME, 8)) {
        memcpy(dg_found_addr, addr, 6);
        dg_found = 1;
    }
}

void dg_bt_pair_start(void)
{
    if (dg_pairing) {
        return;
    }
    gp_log("bt: pairing: searching ~%u s for \"%s\" (a pad on its receiver link, discoverable)",
           DG_INQUIRY_LEN * 128 / 100, DG_PAD_LINK_NAME);
    dg_pairing = 1;
    dg_found = 0;
    if (dg_acl) {
        user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
    }
    user_send_cmd_prepare(USER_CTRL_WRITE_SCAN_DISABLE, 0, NULL);
    user_send_cmd_prepare(USER_CTRL_WRITE_CONN_DISABLE, 0, NULL);
    u8 len = DG_INQUIRY_LEN;
    int ret = user_send_cmd_prepare(USER_CTRL_SEARCH_DEVICE, 1, &len);
    if (ret) {
        gp_log("bt: pairing: the search was refused (%d)", ret);
    }
}

static void dg_bt_pair_end(const char *why)
{
    gp_log("bt: pairing: %s", why);
    dg_pairing = 0;
    if (!dg_acl) {
        dg_bt_connectable();
    }
}

/* App task, every 100 ms: pairing's next step, and reconnecting. */
static void dg_bt_poll(void *priv)
{
    static u16 idle_ms, pair_ms;
    if (!dg_pairing) {
        pair_ms = 0;
    } else if ((pair_ms += 100) >= DG_PAIR_TIMEOUT_MS) {
        user_send_cmd_prepare(USER_CTRL_INQUIRY_CANCEL, 0, NULL);
        dg_bt_pair_end("timed out");
        return;
    }
    if (dg_pairing && dg_found) {
        dg_found = 0;
        user_send_cmd_prepare(USER_CTRL_INQUIRY_CANCEL, 0, NULL);
        user_send_cmd_prepare(USER_CTRL_DEL_ALL_REMOTE_INFO, 0, NULL);   /* the new pad replaces the old */
        dg_bt_log_addr("bt: pairing: connecting to", dg_found_addr);
        user_send_cmd_prepare(USER_CTRL_START_CONNEC_VIA_ADDR, 6, dg_found_addr);
        dg_bt_paging();
        dg_bt_pair_end("pad found");
        idle_ms = 0;
        return;
    }
    if (dg_acl) {
        dg_page_since = 0;
        dg_page_stuck = 0;
    }
    if (dg_pairing || dg_acl) {
        idle_ms = 0;
        return;
    }
    u32 since = dg_page_since;
    if (since) {
        if (jiffies_msec() - since < DG_PAGE_STUCK_MS) {
            return;                                  /* a page is out: its result first */
        }
        dg_page_since = 0;
        if (++dg_page_stuck >= DG_PAGE_STUCK_MAX) {
            gp_log("bt: %u pages in a row never ended: restarting the receiver", dg_page_stuck);
            sys_timeout_add(NULL, dg_bt_restart, 300);   /* the log line goes out first */
            dg_page_since = jiffies_msec() | 1;          /* nothing more meanwhile */
            return;
        }
        gp_log("bt: the page never ended (%u s): cancelling it, connectable again", DG_PAGE_STUCK_MS / 1000);
        user_send_cmd_prepare(USER_CTRL_PAGE_CANCEL, 0, NULL);
        dg_bt_connectable();
        idle_ms = 0;
        return;
    }
    idle_ms += 100;
    if (idle_ms >= DG_RECONNECT_MS) {
        idle_ms = 0;
        if (bt_connect_phone_back_start()) {
            dg_bt_paging();
        } else {
            dg_bt_connectable();                     /* no bond yet: just stay connectable */
        }
    }
}

/* ---- Start-up and events ------------------------------------------------- */
void dg_bt_init(void)
{
    const char *name = "C2 receiver";
    /* The receiver may have no address of its own in its configuration
     * (stock generates one: "Generate Self Bt Addr"); all FF / all 00 is
     * not one. Then a fixed locally administered test address,
     * 02:C2:0D:00:00:01 (library order: least significant byte first). */
    const u8 *own = bt_get_mac_addr();
    int ff = 1, zero = 1;
    for (int i = 0; i < 6; i++) {
        ff &= own[i] == 0xff;
        zero &= own[i] == 0x00;
    }
    if (ff || zero) {
        static u8 test_addr[6] = {0x01, 0x00, 0x00, 0x0D, 0xC2, 0x02};
        bt_set_mac_addr(test_addr);
        gp_log("bt: no address configured: using the test address 02:C2:0D:00:00:01");
    }
    clk_set("sys", BT_NORMAL_HZ);
    bt_pll_para(TCFG_CLOCK_OSC_HZ, clk_get("sys"), 0, 0);
    btstack_edr_start_before_init(&dg_edr_config, 0);
    user_hid_init(dg_bt_hid_in);
    bt_set_local_name((char *)name, strlen(name));
    inquiry_result_handle_register(dg_bt_inquiry_result);
    btstack_init();
    sys_timer_add(NULL, dg_bt_rx_report, 1000);
    sys_timer_add(NULL, dg_bt_poll, 100);
    sys_timer_add(NULL, dg_bt_output_poll, 2);   /* host -> pad: at most 2 ms late */
    gp_log("bt: stack starting as \"%s\"", name);
}

static void dg_bt_log_addr(const char *what, const u8 *a)
{
    /* The library keeps addresses least significant byte first; Linux
     * writes them most significant first. Both shown. */
    gp_log("%s %02X:%02X:%02X:%02X:%02X:%02X (library order %02x %02x %02x %02x %02x %02x)", what,
           a[5], a[4], a[3], a[2], a[1], a[0], a[0], a[1], a[2], a[3], a[4], a[5]);
}

int dg_bt_event(struct sys_event *e)
{
    if (e->type != SYS_BT_EVENT) {
        return 0;
    }
    struct bt_event *bt = &e->u.bt;
    if ((u32)e->arg == SYS_BT_EVENT_TYPE_CON_STATUS) {
        gp_log("bt: status event %d value %d", bt->event, bt->value);
        if (bt->event == BT_STATUS_INIT_OK) {
            dg_bt_log_addr("bt: init ok, own address", bt_get_mac_addr());
            hid_diy_set_sendbuf_size(2 + 64);
            btstack_edr_start_after_init(0);
            if (bt_connect_phone_back_start()) {     /* the bonded pad */
                dg_bt_paging();
                gp_log("bt: reconnecting to the bonded pad");
            } else {
                gp_log("bt: no bonded pad: connectable (press the button to pair)");
                dg_bt_connectable();
            }
            dg_crumb(9);
            return 1;
        }
        if (bt->event == BT_STATUS_INQUIRY_TIMEOUT && dg_pairing) {
            dg_bt_pair_end("no pad found (is it in pairing mode?)");
        }
        bt_comm_edr_status_event_handler(bt);
    } else if ((u32)e->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
        gp_log("bt: hci event %02x value %02x", bt->event, bt->value);
        int failed = bt->event == HCI_EVENT_CONNECTION_COMPLETE && bt->value != ERROR_CODE_SUCCESS;
        if (bt->event == HCI_EVENT_CONNECTION_COMPLETE) {
            dg_page_since = 0;                   /* the page (or the pad's) has its result */
            if (!failed) {
                dg_page_stuck = 0;
            }
        }
        if (bt->event == HCI_EVENT_CONNECTION_COMPLETE && bt->value == ERROR_CODE_SUCCESS) {
            dg_acl = 1;
        } else if (bt->event == HCI_EVENT_DISCONNECTION_COMPLETE || failed) {
            dg_acl = 0;
            dg_hid = 0;
            dg_link_reset();
            dg_usb_pad_gone();                   /* USB: no gamepad until a pad speaks again */
        }
        if (failed) {
            /* Not the SDK's handler: it pages again at once on a Connection
             * Timeout (that page never ended, see DG_PAGE_STUCK_MS) and makes
             * us discoverable on a page timeout. Connectable; dg_bt_poll pages
             * again in DG_RECONNECT_MS. */
            if (!dg_pairing) {
                dg_bt_connectable();
            }
            return 1;
        }
        bt_comm_edr_hci_event_handler(bt);
        if (bt->event == HCI_EVENT_DISCONNECTION_COMPLETE && !dg_pairing) {
            user_send_cmd_prepare(USER_CTRL_WRITE_SCAN_DISABLE, 0, NULL);   /* the SDK's handler made us discoverable */
        }
    }
    return 1;
}

/* ---- Debug-log commands ("bt ...") --------------------------------------- */
static int dg_hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "AA:BB:CC:DD:EE:FF" (Linux order) -> the library's order (no sscanf here). */
static int dg_parse_addr(const char *s, u8 out[6])
{
    for (int i = 0; i < 6; i++) {
        int hi = dg_hex(s[0]), lo = dg_hex(s[1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[2] != ':')) {
            return 0;
        }
        out[5 - i] = hi << 4 | lo;
        s += 3;
    }
    return 1;
}

int dg_bt_command(const char *cmd)
{
    if (!strncmp(cmd, "bt connect ", 11)) {
        static u8 addr[6];
        if (!dg_parse_addr(cmd + 11, addr)) {
            gp_log("cmd: bt connect AA:BB:CC:DD:EE:FF (as Linux writes it)");
            return 1;
        }
        dg_bt_log_addr("cmd: paging", addr);
        user_send_cmd_prepare(USER_CTRL_START_CONNEC_VIA_ADDR, 6, addr);
        dg_bt_paging();
        return 1;
    }
    if (!strcmp(cmd, "bt hid")) {
        gp_log("cmd: asking for the HID channels");
        user_send_cmd_prepare(USER_CTRL_HID_CONN, 0, NULL);
        return 1;
    }
    if (!strcmp(cmd, "bt pair")) {
        dg_bt_pair_start();
        return 1;
    }
    if (!strcmp(cmd, "stats")) {
        dg_stats_once = 1;
        return 1;
    }
    if (!strcmp(cmd, "stats on") || !strcmp(cmd, "stats off")) {
        dg_stats_on = cmd[7] == 'n';
        gp_log("cmd: stats every second %s", dg_stats_on ? "on" : "off");
        return 1;
    }
    if (!strcmp(cmd, "bt disc")) {
        gp_log("cmd: disconnecting");
        user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
        return 1;
    }
    return 0;
}

#endif
