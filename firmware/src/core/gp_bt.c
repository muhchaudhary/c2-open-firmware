/* gp_bt.c: see gp_bt.h. */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "btcontroller_config.h"
#include "btstack/avctp_user.h"
#include "btstack/btstack_task.h"
#include "bt_common.h"
#include "edr_hid_user.h"
#include "app_comm_bt.h"
#include "gamepad.h"
#include "gp_bt.h"
#include "gp_sinput.h"
#include "gp_config.h"
#include "gp_xboxbt.h"
#include "gp_ds4.h"
#include "gp_switch.h"
#include "gp_link.h"
#include "gp_core.h"
#include "gp_settings.h"
#include "gp_power.h"
#include "gp_log.h"

extern void bt_set_local_name(char *name, u8 len);   /* apps/hid/modules/user_cfg.c (no header) */
extern int edr_hid_is_busy(void);                     /* apps/hid/modules/bt/edr_hid_user.c (no header) */
extern void edr_hid_send_unstick(void);               /* patch_sdk.py */
extern void user_hid_regiser_wakeup_send(void *cbk);
extern unsigned long jiffies_msec(void);
extern void vTaskSuspendAll(void);
extern long xTaskResumeAll(void);
extern void gp_sdp_set_identity(u16 vid, u16 pid, u16 version, const char *name);   /* patch_sdk.py */
extern void bt_set_mac_addr(u8 *addr);                                              /* user_cfg.c */
extern void set_bt_vm_interface(u32 vm_index, void *interface);                     /* btstack.a */
extern void *bt_vm_interface(void);                                                 /* cpu.a */

/* Xbox mode, as stock's Bluetooth Xbox mode (C2 v3.56 mode 2: its EIR at
 * 0x1E63E2A, Device ID 045E:02FD v1.00, the HID record at 0x1E654DB): an
 * Xbox One S controller, which Windows' built-in driver takes. Like stock
 * ("set xinput bt addr", "write bt xinput link key") it is a device of its
 * own to the host: another Bluetooth address, and its own bonds (the
 * library's remote-device store moved from VM id 62 to XBOX_BOND_VM, in
 * the range the SDK leaves free on this build: VM_ITEM_MAX_NUM 256, BLE and
 * mesh off). So a host keeps both pairings and each mode reconnects to its
 * own hosts. */
#define XBOX_VID            0x045E
#define XBOX_PID            0x02FD
#define XBOX_VERSION        0x0100
#define XBOX_NAME           "Xbox Bluetooth Gamepad"
#define XBOX_BOND_VM        130             /* 21 items: 16 bonds + the library's index */

/* The receiver link (gp_settings.link, docs/dongle.md): the pad's own
 * identity for the C2 receiver, as Xbox mode has one for its hosts: another
 * address (bit 1 of the low byte flipped; Xbox mode flips bit 0), its own
 * bonds (VM ids RECEIVER_BOND_VM..+20, after Xbox mode's) and a name the
 * receiver's pairing looks for (and nothing else answers to). So the
 * receiver and Bluetooth hosts each keep their pairing, and switching the
 * link (Home + M held) disturbs neither. The
 * same identity in either mode: on this link the mode is the receiver's
 * business (what it shows on USB), not the pad's Bluetooth identity. */
#define RECEIVER_BOND_VM    160
#define RECEIVER_LINK_NAME  "C2 pad (receiver link)"

/* DS4 mode (gp_ds4.h): a DualShock 4 to its hosts, as stock's PS4 mode is
 * a device of its own: another address (bit 2 of the low byte flipped),
 * its own bonds (VM ids DS4_BOND_VM..+20, after the receiver link's), the
 * DS4's Device ID and name. */
#define DS4_BOND_VM         190

/* Switch mode (gp_switch.h): a Pro Controller, as stock's "bt switch mode"
 * is a device of its own: another address (bit 3 flipped), its own bonds
 * (VM ids SWITCH_BOND_VM..+20, after DS4 mode's; the last free 21 below
 * VM_ITEM_MAX_NUM 256), the name "Pro Controller" and 057E:2009. The class
 * of device is already a gamepad's, 0x002508, as a real one. */
#define SWITCH_BOND_VM      220

int gp_bt_receiver(void)
{
    return gp_settings.link == GP_LINK_RECEIVER;
}

static int gp_bt_xbox(void)
{
    return !gp_bt_receiver() && gp_settings.mode == GP_MODE_XBOX;
}

static int gp_bt_ds4(void)
{
    return !gp_bt_receiver() && gp_settings.mode == GP_MODE_DS4;
}

static int gp_bt_switch(void)
{
    return !gp_bt_receiver() && gp_settings.mode == GP_MODE_SWITCH;
}

/* The BT library's interrupt-channel send buffer defaults to 64 bytes
 * (hid.c's diy_data_buf_size; hid_diy_data_try_send returns 3 for anything
 * longer), and a report goes in as [0xA1][ID][63 B] = 65 B, so every SInput
 * report was dropped. The setter takes up to l2cap_max_mtu() - 4; the buffer
 * is malloc'd at the next connection with the new size (hid_diy_state_sync),
 * so this must run before a host connects. Read from the library's bitcode
 * (cpu/br23/liba/btstack.a, hid.c.o). */
extern int hid_diy_set_sendbuf_size(u16 size);

/* The library's L2CAP send (btstack.a; hid_ctrl_try_send's call): 0 when
 * sent. gp_bt_get_report's replies don't fit the HID control buffer. */
extern int l2cap_send_internal(u16 local_cid, const u8 *data, u16 len);

/* ---- The stack's configuration ---------------------------------------------
 * Sniff: the short anchor profile (10 ms), as the SDK uses for mice and
 * keyboards. (patch_sdk.py also turns device-initiated sniff off: a gamepad
 * reports continuously.) */
static const edr_sniff_par_t gp_sniff_param = {
    .sniff_mode = SNIFF_MODE_ANCHOR,
    .cnt_time = 1,
    .max_interval_slots = 16,
    .min_interval_slots = 16,
    .attempt_slots = 2,
    .timeout_slots = 1,
    .check_timer_period = 200,
};

static edr_init_cfg_t gp_edr_config = {
    .page_timeout = 8000,
    .super_timeout = 8000,
    .io_capabilities = 3,
    .passkey_enable = 0,
    .authentication_req = 2,
    .oob_data = 0,
    .sniff_param = &gp_sniff_param,
    .class_type = BD_CLASS_GAMEPAD,
    .report_map = sinput_report_desc,
    .report_map_size = SINPUT_REPORT_DESC_LEN,
};

static volatile u8 gp_acl_up;           /* ACL link up (HCI events) */
static u8 gp_pairing_on;
static u8 gp_suspended;                /* wired: Bluetooth stays out of the way */
static u8 gp_recover_reconnect;         /* we dropped the link to recover: reconnect */
u32 gp_bt_sent;
volatile int gp_hid_send_err;
volatile u32 gp_hid_send_errs;
static volatile u32 gp_tx_done_total;

/* The report in flight (the safety nets in gp_bt_poll use it). */
static struct {
    volatile u32 t_send;
    volatile u8 in_flight;
} gp_tx;

/* ---- Output report 0x03 from the host. The packet may come with or without
 * the HIDP header byte (0xA2 DATA|OUTPUT, or 0x5x SET_REPORT): skip it. */
static void gp_bt_hid_output(u8 *packet, u16 size, u16 channel)
{
    u8 *p = packet;
    if (size >= 2 && (p[0] == 0xa2 || (p[0] & 0xf0) == 0x50)) {
        p++;
        size--;
    }
    if (gp_bt_receiver() && size >= 1 && p[0] == GP_LINK_ID_HELLO_REQ) {
        gp_core_link_hello();                            /* the receiver has no hello yet */
        return;
    }
    if (gp_bt_xbox()) {
        if (size >= 2 && p[0] == XBOXBT_ID_RUMBLE) {
            xboxbt_command(p + 1, size - 1);
        }
        return;
    }
    if (gp_bt_ds4()) {
        ds4_output(p, size);                             /* 0x11: rumble, lightbar */
        return;
    }
    if (gp_bt_switch()) {
        sw_output(&sw_bt_ctx, p, size);                  /* 0x01 rumble + subcommand, 0x10 rumble */
        return;
    }
    if (size < 2 || p[0] != SINPUT_ID_COMMAND) {
        return;
    }
    sinput_command(p + 1, size - 1);
}

/* GET_REPORT on the HID control channel (patch_sdk.py calls this from the
 * SDK's hid_ctrl_data_parse_hook, in the BT stack's task): DS4 mode's
 * feature reports, which hosts read at the connect (hid-playstation: the
 * firmware info 0xA3, then the calibration 0x05; without them it turns
 * the IMU calibration off). The reply is DATA | FEATURE, the report ID, the
 * report, sent here: the library copies a hook's reply into the
 * connection's 16-byte control buffer (malloc(16) in __create_hid_conn) and
 * drops anything longer without a word, so the host timed out (-EIO) on
 * every DS4 feature report. Anything else: 0, the library answers as
 * before. */
int gp_bt_get_report(u16 cid, const u8 *packet, int size)
{
    static u8 buf[1 + DS4_FEAT_MAX_LEN];
    if (!gp_bt_ds4() || size < 2 || (packet[0] & 0x03) != 0x03) {
        return 0;
    }
    const u8 *mac = bt_get_mac_addr();
    u8 msb[6];
    for (int k = 0; k < 6; k++) {
        msb[k] = mac[5 - k];
    }
    int n = ds4_feature(packet[1], 1, msb, buf + 1);
    if (!n) {
        gp_log("ds4: GET_REPORT feature %02x: none", packet[1]);
        return 0;
    }
    buf[0] = 0xa3;                                       /* HIDP DATA | Feature */
    int err = l2cap_send_internal(cid, buf, 1 + n);
    if (err) {
        gp_log("ds4: GET_REPORT feature %02x: send failed %d", packet[1], err);
    }
    return 1;
}

void gp_bt_init(void)
{
    const char *name = gp_board_info.name;
    if (gp_bt_receiver()) {
        u8 mac[6];
        memcpy(mac, bt_get_mac_addr(), 6);
        mac[0] ^= 0x02;                                  /* a device of its own to the receiver */
        bt_set_mac_addr(mac);
        name = RECEIVER_LINK_NAME;
        gp_sdp_set_identity(GP_PNP_VID, GP_PNP_PID, GP_PNP_VERSION, RECEIVER_LINK_NAME);
    } else if (gp_bt_xbox()) {
        u8 mac[6];
        memcpy(mac, bt_get_mac_addr(), 6);
        mac[0] ^= 0x01;                                  /* a device of its own to the host */
        bt_set_mac_addr(mac);
        name = XBOX_NAME;
        gp_sdp_set_identity(XBOX_VID, XBOX_PID, XBOX_VERSION, XBOX_NAME);
        gp_edr_config.report_map = xboxbt_report_desc;
        gp_edr_config.report_map_size = XBOXBT_REPORT_DESC_LEN;
    } else if (gp_bt_ds4()) {
        u8 mac[6];
        memcpy(mac, bt_get_mac_addr(), 6);
        mac[0] ^= 0x04;                                  /* a device of its own to the host */
        bt_set_mac_addr(mac);
        name = DS4_NAME;
        gp_sdp_set_identity(DS4_VID, DS4_PID, DS4_VERSION, DS4_NAME);
        gp_edr_config.report_map = ds4_bt_report_desc;
        gp_edr_config.report_map_size = ds4_bt_report_desc_len;
    } else if (gp_bt_switch()) {
        u8 mac[6];
        memcpy(mac, bt_get_mac_addr(), 6);
        mac[0] ^= 0x08;                                  /* a device of its own to the host */
        bt_set_mac_addr(mac);
        name = SW_NAME;
        gp_sdp_set_identity(SW_VID, SW_PID, SW_VERSION, SW_NAME);
        gp_edr_config.report_map = sw_bt_report_desc;
        gp_edr_config.report_map_size = sw_bt_report_desc_len;
    } else {
        gp_sdp_set_identity(GP_PNP_VID, GP_PNP_PID, GP_PNP_VERSION, GP_PRODUCT_NAME);
    }
    gp_log("bt: %s as \"%s\"", gp_bt_receiver() ? "receiver link" : gp_bt_xbox() ? "Xbox mode" :
           gp_bt_ds4() ? "DS4 mode" : gp_bt_switch() ? "Switch mode" : "Steam mode", name);
    btstack_edr_start_before_init(&gp_edr_config, 0);
    user_hid_init(gp_bt_hid_output);
    bt_set_local_name((char *)name, strlen(name));
    btstack_init();
}

void gp_bt_after_init(void)
{
    if (gp_bt_receiver()) {
        set_bt_vm_interface(RECEIVER_BOND_VM, bt_vm_interface());   /* the receiver link's own bonds */
    } else if (gp_bt_xbox()) {
        set_bt_vm_interface(XBOX_BOND_VM, bt_vm_interface());   /* Xbox mode's own bonds */
    } else if (gp_bt_ds4()) {
        set_bt_vm_interface(DS4_BOND_VM, bt_vm_interface());    /* DS4 mode's own bonds */
    } else if (gp_bt_switch()) {
        set_bt_vm_interface(SWITCH_BOND_VM, bt_vm_interface()); /* Switch mode's own bonds */
    }
    btstack_edr_start_after_init(0);
    if (!bt_connect_phone_back_start()) {
        bt_wait_phone_connect_control(1);
    }
}

static void gp_tx_done(void)
{
    if (!gp_tx.in_flight) {
        return;
    }
    gp_tx.in_flight = 0;
    gp_tx_done_total++;
}

int gp_bt_start(void)
{
    user_hid_regiser_wakeup_send(gp_tx_done);            /* send-done: the safety nets */
    if (!hid_diy_set_sendbuf_size(2 + GP_BT_REPORT_MAX)) {
        gp_log("ERROR hid_diy_set_sendbuf_size refused");
        return 0;
    }
    return 1;
}

/* ---- Sending ----------------------------------------------------------------
 * Sending happens in the app task, not in the tick. The tick runs in the
 * hardware timer interrupt (usr_timer), and edr_hid_data_send ends in
 * user_send_cmd_prepare(USER_CTRL_HID_SEND_DATA), a BT-stack call, while the
 * BT task runs the same send path from its send-done callback (and
 * bt_send_busy is a plain check-then-set). The pad reset by itself every so
 * often while connected (rst 04). The tick copies the report into gp_out
 * and posts one event; the app task (gp_bt_send_posted) sends the newest
 * copy. */
#define GP_EVENT_REPORT     0xC2
static u8 gp_out[GP_BT_REPORT_MAX];
static volatile u8 gp_out_id;           /* 0: nothing waiting, else the report ID */
static volatile u8 gp_out_len;

static int gp_bt_connected(void)
{
    return edr_hid_is_connected() != 0;
}

static int gp_bt_busy(void)
{
    return edr_hid_is_busy();
}

static int gp_bt_protocol(void)
{
    return gp_bt_receiver() ? GP_PROTO_LINK : gp_bt_xbox() ? GP_PROTO_XBOXBT :
           gp_bt_ds4() ? GP_PROTO_DS4BT : gp_bt_switch() ? GP_PROTO_SWITCHBT : GP_PROTO_SINPUT;
}

static void gp_bt_post(u8 id, const u8 *report, int len)
{
    static u8 waiting;                                   /* ticks the posted event has gone unanswered */
    u8 posted = gp_out_id != 0;
    /* Report 2 still waiting is never replaced by another report. SInput:
     * the features reply (SDL gives up on the pad, generic driver: no
     * sticks, no gyro, if the reply to its request doesn't come within
     * 100 ms). Xbox: the Xbox button. */
    /* A configuration reply waiting (also report 2, gp_config.h) isn't
     * replaced by anything: an unasked features reply took its place and
     * the reply was lost. */
    /* Link (receiver): a hello or status waiting isn't replaced by a state
     * report either (the next tick's state goes after it). */
    int keep = gp_out_id == 2 && (id != 2 || gp_out[0] == GP_CFG_CMD);
    keep |= (gp_out_id == GP_LINK_ID_HELLO || gp_out_id == GP_LINK_ID_STATUS) && id == GP_LINK_ID_STATE;
    /* SInput's battery report (sent only on a change) isn't replaced by a
     * state report either. Xbox mode's battery is ID 4 too: the same holds. */
    keep |= gp_out_id == SINPUT_ID_BATTERY && id == SINPUT_ID_STATE;
    /* Switch: a subcommand's reply waiting isn't replaced by anything (the
     * host waits for it); a newer reply that finds it there goes again on
     * the next tick (gp_switch.h: sw_reply_unsent). */
    if (gp_bt_switch()) {
        keep = gp_out_id == SW_ID_REPLY;
        if (keep && id == SW_ID_REPLY) {
            sw_reply_unsent(&sw_bt_ctx);
        }
    }
    if (!keep) {
        memcpy(gp_out, report, MIN(len, sizeof(gp_out)));
        gp_out_len = MIN(len, sizeof(gp_out));
        gp_out_id = id;
    }
    if (posted && ++waiting >= 5) {
        posted = 0;                                      /* the event may have been dropped: post again */
    }
    if (!posted) {
        waiting = 0;
        struct sys_event e;
        e.type = SYS_DEVICE_EVENT;
        e.arg = (void *)DEVICE_EVENT_FROM_CUSTOM;
        e.u.dev.event = GP_EVENT_REPORT;
        e.u.dev.value = 0;
        sys_event_notify(&e);
    }
}

const struct gp_transport gp_bt_transport = {
    .name = "bt",
    .connected = gp_bt_connected,
    .busy = gp_bt_busy,
    .protocol = gp_bt_protocol,
    .post = gp_bt_post,
};

/* ---- Reports straight to the controller, as stock does ---------------------
 * The library's path (edr_hid_data_send -> user command ring -> btstack task
 * -> L2CAP) holds one report until the stack's send-done, which came back
 * ~10-12 ms later here: ~85-110 reports/s, intervals up to 40 ms
 * (tools/c2rate.py, 2026-11-14). Stock v19 (~300/s on the same PC) builds
 * the whole ACL packet itself and hands it to the controller (0x1E032A0:
 * the HCI transport with type 2, ACL). The SDK's equivalent, read from the
 * libraries' bitcode (fw-re/sdk/sdklib.sh): hci_get_outgoing_acl_packet_buffer
 * (hci_vendor.c) takes a buffer from the controller's TX pool (NULL when it
 * is full: skip this report) and bredr_hci_send_acl_packet (hci_vendor.c ->
 * lmp_hci_send_packet, lmp_rxtx.c) queues it on the link by its handle. The
 * L2CAP channel (l2cap.c, l2cap_send_prepared): handle u16 at +4, PSM at
 * +28, local CID at +30, remote CID at +32. */
#define HID_PSM_INTERRUPT   0x13
extern u16 l2cap_get_local_cid_via_psm(u16 psm);
extern u16 l2cap_get_hci_handle_via_psm(u16 psm);
extern u8 *l2cap_get_channel_for_local_cid(u16 local_cid);
extern u8 *hci_get_outgoing_acl_packet_buffer(u16 acl_len);
extern int bredr_hci_send_acl_packet(u8 *packet, int size);

/* 0 sent, 1 TX pool full, -1 no interrupt channel. Scheduler held. */
static int gp_bt_raw_send(u8 id, const u8 *data, int len)
{
    u16 lcid = l2cap_get_local_cid_via_psm(HID_PSM_INTERRUPT);
    u8 *ch = lcid ? l2cap_get_channel_for_local_cid(lcid) : NULL;
    if (!ch) {
        return -1;
    }
    u16 handle = ch[4] | ch[5] << 8;
    u16 rcid = ch[32] | ch[33] << 8;
    u16 l2len = 2 + len;                                 /* HIDP header + report ID + data */
    u16 acl_len = 4 + l2len;                             /* + the L2CAP header */
    u8 *p = hci_get_outgoing_acl_packet_buffer(acl_len);
    if (!p) {
        return 1;
    }
    p[0] = handle;
    p[1] = (handle >> 8) | 0x20;                         /* first automatically-flushable packet */
    p[2] = acl_len;
    p[3] = acl_len >> 8;
    p[4] = l2len;
    p[5] = l2len >> 8;
    p[6] = rcid;
    p[7] = rcid >> 8;
    p[8] = 0xa1;                                         /* HIDP DATA | Input */
    p[9] = id;
    memcpy(p + 10, data, len);
    bredr_hci_send_acl_packet(p, 4 + acl_len);
    return 0;
}

/* A features reply that couldn't go out: again on the next free tick, as
 * a reply to the host's request if it was one, else as an unasked one. */
static void gp_feat_retry(const u8 *rep)
{
    if (rep[0] == GP_CFG_CMD) {
        gp_config_retry();                               /* a configuration reply, not a features one */
    } else if (sinput_feat_asked) {
        sinput_feat_pending = 1;
    } else {
        sinput_feat_retry = 1;
    }
}

/* App task: the event from gp_bt_post. */
static void gp_bt_send_posted(void)
{
    u8 buf[sizeof(gp_out)], id, len;
    OS_ENTER_CRITICAL();                                 /* the tick may be writing a newer one */
    id = gp_out_id;
    len = gp_out_len;
    memcpy(buf, gp_out, sizeof(buf));
    gp_out_id = 0;
    OS_EXIT_CRITICAL();
    if (id && edr_hid_is_connected()) {
        /* Link state: the sequence number goes on here, and only a report
         * that went out takes one, so the receiver's "lost" counts reports
         * lost on the way, not ones skipped because the controller's pool
         * was full (the pad makes ~400 a second while the sticks move, the
         * link carries ~290). */
        static u8 link_seq;
        if (id == GP_LINK_ID_STATE) {
            buf[0] = link_seq;
        }
        vTaskSuspendAll();
        int r = gp_bt_raw_send(id, buf, len);
        xTaskResumeAll();
        if (r == 0 && id == GP_LINK_ID_STATE) {
            link_seq++;
        }
        if (r >= 0) {
            if (r == 0) {
                gp_bt_sent++;
                gp_tx_done_total++;                      /* no send-done on this path */
                if (gp_bt_xbox()) {
                    if (id == XBOXBT_ID_GUIDE) {
                        xboxbt_guide_dirty = 0;
                    }
                } else if (id == SINPUT_ID_FEATURES && buf[0] != GP_CFG_CMD && sinput_feat_asked) {
                    sinput_feat_asked = 0;
                    gp_log("hid: features reply sent (asked)");
                }
            } else if (gp_bt_switch()) {
                if (id == SW_ID_REPLY) {
                    sw_reply_unsent(&sw_bt_ctx);         /* pool full: again on the next tick */
                }
            } else if (id == GP_LINK_ID_HELLO || id == GP_LINK_ID_STATUS) {
                gp_core_link_unsent(id);                 /* pool full: again on the next tick */
            } else if (!gp_bt_xbox() && id == SINPUT_ID_FEATURES) {
                /* Pool full: the next tick sends it again. Right after a
                 * connect the pool is full most of the time, and SDL's
                 * request comes then: dropping the unasked replies left SDL
                 * with none in its 100 ms (generic driver, no gyro). An
                 * unasked one is retried as unasked: re-pending it as a
                 * request logged ~170 "asked" replies the host never asked
                 * for. */
                gp_feat_retry(buf);
            }
            return;
        }
    }
    if (id && edr_hid_is_connected() && !edr_hid_is_busy()) {   /* no channel found: the library's path */
        gp_tx.t_send = jiffies_msec();
        gp_tx.in_flight = 1;
        /* The BT library's user command ring (user_send_cmd_prepare,
         * 0x1e1072e in this build) has no lock: it writes tail + 1 (which
         * can be 16) before storing the command, and only then wraps it to
         * 0. If the btstack task (priority 3) preempts the app task (1) in
         * between, it sees head != 16, runs round the ring for ever (head
         * wraps 0..15), and never lets the app task finish the write: the
         * "stall" (2026-11-13: 14 s, 19 s, > 30 s; stock, which builds its
         * own packets, never stalls). Every report goes through that ring.
         * The HID send path never sleeps, so the scheduler can be held for
         * it. (Not done for disconnect / connect commands: they may sleep.) */
        vTaskSuspendAll();
        edr_hid_data_send(id, buf, len);                 /* 0 = queued; the stack's own result: gp_hid_send_err */
        xTaskResumeAll();
        gp_bt_sent++;
        if (gp_bt_xbox()) {
            if (id == XBOXBT_ID_GUIDE) {
                xboxbt_guide_dirty = 0;                  /* else the tick sends it again */
            }
        } else if (id == SINPUT_ID_FEATURES && buf[0] != GP_CFG_CMD && sinput_feat_asked) {
            sinput_feat_asked = 0;
            gp_log("hid: features reply sent (asked)");
        }
    } else if (!gp_bt_xbox() && id == SINPUT_ID_FEATURES && edr_hid_is_connected()) {
        gp_feat_retry(buf);                              /* busy: the next free tick sends it again */
    }
}

/* ---- Stack events -------------------------------------------------------- */
static void gp_bt_hci_event(struct bt_event *bt)
{
    gp_log("bt: hci event %x %x", bt->event, bt->value);
    if (bt->event == HCI_EVENT_CONNECTION_COMPLETE && bt->value == ERROR_CODE_SUCCESS) {
        gp_acl_up = 1;
    } else if (bt->event == HCI_EVENT_DISCONNECTION_COMPLETE ||
               (bt->event == HCI_EVENT_CONNECTION_COMPLETE && bt->value != ERROR_CODE_SUCCESS)) {
        gp_acl_up = 0;
    }
    bt_comm_edr_hci_event_handler(bt);
    if (gp_power_off_pending() || gp_suspended) {
        bt_wait_phone_connect_control_ext(0, 0);         /* undo the handler's "connectable again" */
        if (gp_acl_up) {
            user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);   /* the host got back in: out again */
        }
    }
}

int gp_bt_event(struct sys_event *e)
{
    if (e->type == SYS_BT_EVENT) {
        if ((u32)e->arg == SYS_BT_EVENT_TYPE_CON_STATUS) {
            gp_log("bt: status event %d", e->u.bt.event);
            bt_comm_edr_status_event_handler(&e->u.bt);
        } else if ((u32)e->arg == SYS_BT_EVENT_TYPE_HCI_STATUS) {
            gp_bt_hci_event(&e->u.bt);
        }
        return 1;
    }
    if (e->type == SYS_DEVICE_EVENT && (u32)e->arg == DEVICE_EVENT_FROM_CUSTOM &&
        e->u.dev.event == GP_EVENT_REPORT) {
        gp_bt_send_posted();
        return 1;
    }
    return 0;
}

/* ---- Link helpers for the power-off ------------------------------------- */
int gp_bt_link_up(void)
{
    return gp_acl_up || get_bt_connect_status() >= BT_STATUS_CONNECTING;
}

void gp_bt_disconnect(void)
{
    user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
}

void gp_bt_scan_off(void)
{
    bt_wait_phone_connect_control_ext(0, 0);
}

void gp_bt_shutdown(void)
{
    btstack_edr_exit(0);
}

int gp_bt_pairing(void)
{
    return gp_pairing_on;
}

/* While wired the host must not see the pad twice: drop the link and stay
 * out of reach (the HCI handler keeps it so); unplugged, reconnect to the
 * last host as at start-up. */
void gp_bt_suspend(int on)
{
    if (on == gp_suspended) {
        return;
    }
    gp_suspended = on;
    gp_pairing_on = 0;
    gp_recover_reconnect = 0;
    if (on) {
        if (gp_bt_link_up()) {
            gp_bt_disconnect();
        }
        gp_bt_scan_off();
    } else if (!bt_connect_phone_back_start()) {
        bt_wait_phone_connect_control(1);
    }
}

/* ---- Poll ---------------------------------------------------------------- */

/* HID channel retry. On a reconnect the pad pages the host and the library
 * opens the HID control channel (PSM 0x11) right after its SDP query,
 * before the link is encrypted; BlueZ refuses that with "security block"
 * (btmon, 2026-11-12: refused at +0.74 s, encryption on at +0.88 s) and the
 * library never tries again: the host shows the pad connected, no HID, no
 * input. So: ACL up without HID for HID_RETRY_MS -> ask for the HID
 * connection again (the link is encrypted by then); after HID_RETRIES
 * tries, drop the link so the next attempt starts clean. */
#define HID_RETRY_MS        1500
#define HID_RETRIES         3

static void gp_bt_hid_retry(u8 connected)
{
    static u16 ms;
    static u8 tries;
    if (connected || !gp_acl_up) {
        ms = 0;
        tries = 0;
        return;
    }
    if ((ms += GP_POLL_MS) < HID_RETRY_MS) {
        return;
    }
    ms = 0;
    if (tries < HID_RETRIES) {
        tries++;
        gp_log("ACL without HID: HID connect, try %d", tries);
        user_send_cmd_prepare(USER_CTRL_HID_CONN, 0, NULL);
    } else {
        gp_log("ACL without HID: dropping the link");
        user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
        tries = 0;
    }
}

/* After a BT-stack stall the stack may take reports and never complete one
 * (2026-11-13: send-done 0 while ~130 reports a second went in). No send
 * completing for 20 s while sending: drop the link and reconnect (a last
 * resort; the stall itself is fixed in gp_bt_send_posted). */
static void gp_bt_tx_watch(u8 connected)
{
    static u32 done_seen, sent_seen, since_ms;
    u32 now_ms = jiffies_msec();
    if (!connected || gp_tx_done_total != done_seen || gp_bt_sent == sent_seen) {
        done_seen = gp_tx_done_total;
        sent_seen = gp_bt_sent;
        since_ms = now_ms;
    } else if ((s32)(now_ms - since_ms) > 20000 && gp_bt_sent - sent_seen > 50) {
        gp_log("tx: %u reports sent, none completed in %u ms (send failures %u, last %d): dropping the link, then reconnecting",
               (unsigned)(gp_bt_sent - sent_seen), (unsigned)(now_ms - since_ms),
               (unsigned)gp_hid_send_errs, gp_hid_send_err);
        user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
        gp_recover_reconnect = 1;
        sent_seen = gp_bt_sent;
        since_ms = now_ms + 10000;                       /* not again for 30 s */
    } else if ((s32)(now_ms - since_ms) > 1000 && gp_bt_sent - sent_seen > 50) {
        static u32 said;
        if (now_ms - said > 1000) {                      /* the run-up, once a second */
            said = now_ms;
            gp_log("tx: no report completed for %u ms (send failures %u, last %d)",
                   (unsigned)(now_ms - since_ms), (unsigned)gp_hid_send_errs, gp_hid_send_err);
        }
    }
    /* After our own drop neither the host nor the SDK reconnects (it sat
     * disconnected for 49 s, 2026-11-13): page the host ourselves once the
     * link is down. */
    static u32 down_ms;
    if (gp_recover_reconnect && !gp_acl_up && !connected) {
        if (!down_ms) {
            down_ms = now_ms;
        } else if (now_ms - down_ms > 1000) {
            gp_recover_reconnect = 0;
            down_ms = 0;
            gp_log("tx: link down after the recovery drop, reconnecting");
            bt_connect_phone_back_start();
        }
    }
    /* A send that never completed leaves the SDK's busy flag set for good
     * (2026-11-13: after a 19 s BT-stack stall, connected, no input). */
    if (connected && gp_tx.in_flight && edr_hid_is_busy() && (s32)(now_ms - gp_tx.t_send) > 250) {
        gp_log("tx: a send stuck %u ms, clearing busy", (unsigned)(jiffies_msec() - gp_tx.t_send));
        gp_tx.in_flight = 0;
        edr_hid_send_unstick();
    }
}

/* Pairing mode (Home + Capture held, gp_core.c): forgets the remembered
 * hosts of this mode or link (so the controller stops reconnecting to them), drops
 * the connection if there is one, and turns discoverable + connectable on
 * (the SDK's disconnect handler does that too). Home flashes until a host
 * connects. Poll context: forgetting the hosts writes flash. */
void gp_bt_pairing_start(void)
{
    if (gp_suspended) {
        gp_log("pairing mode: not while the gamepad is on USB");
        return;
    }
    gp_log("pairing mode");
    gp_pairing_on = 1;
    gp_power_restart_idle();                             /* the pairing time-out starts now */
    user_send_cmd_prepare(USER_CTRL_DEL_ALL_REMOTE_INFO, 0, NULL);
    if (edr_hid_is_connected()) {
        user_send_cmd_prepare(USER_CTRL_DISCONNECTION_HCI, 0, NULL);
    }
    bt_wait_phone_connect_control(1);
}

void gp_bt_poll(u8 connected)
{
    static u8 was_connected;
    if (gp_suspended) {
        static u16 ms;
        if (gp_acl_up && (ms += GP_POLL_MS) >= 1000) {
            ms = 0;
            gp_bt_disconnect();                          /* a host got in anyway */
        }
        return;
    }
    if (connected && !was_connected) {
        gp_pairing_on = 0;                               /* a host connected */
    }
    was_connected = connected;
    gp_bt_hid_retry(connected);
    gp_bt_tx_watch(connected);
}
