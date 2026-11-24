/* dg_usb.c: the receiver's USB device. A composite of
 *  - the debug log (CDC, core/gp_log.c), with its "boot" command, and
 *  - the gamepad, in the pad's mode (its link hello; saved here, so the
 *    receiver comes up as it at power-up):
 *    - Steam: a HID interface, the SInput gamepad (the pad's descriptor,
 *      the pad's IDs 2E8A:10C6, which is how SDL picks its SInput driver),
 *      with a vendor collection appended laid out as GameSir Connect's
 *      (usage page 0xFFF0, output report 0x0F, 63 bytes): `0F 17 55 88`
 *      enters the USB bootloader (fw-re/enter_bootloader.py);
 *    - Xbox: a wired Xbox 360 pad's interface, the pad's own wired Xbox
 *      mode's (core/gp_xinput_desc.c), under stock's receiver ID 3537:100B,
 *      with the Microsoft OS descriptors Windows binds its Xbox driver by.
 *      (No HID vendor report here: the bootloader is the log's `boot`, the
 *      button at power-up, the crash-loop guard.)
 *    - DS4: a DualShock 4 as the pad is on its own cable (core/gp_usb.c):
 *      054C:09CC, the DS4's report descriptor (the vendor collection after
 *      it, `0F 17 55 88` as in Steam mode), an interrupt OUT endpoint
 *      (hid-playstation sends rumble and lightbar only there), the feature
 *      reports hosts read at the connect (GET_REPORT, the pad's address from
 *      its hello). The host's output -> the pad's SInput commands.
 *    - Switch: a Pro Controller as the pad is on its own cable: 057E:2009,
 *      the Pro Controller's report descriptor alone (no vendor collection:
 *      exactly a real one's for the console), interrupt OUT. The protocol
 *      (core/gp_switch_desc.c) runs here: the host's 80 handshake and
 *      subcommands are answered by the receiver, the state reports built
 *      from the pad's link state (dg_link.c); rumble and player lights go
 *      to the pad as SInput commands.
 *    A mode change re-enumerates (dg_usb_poll, the app task).
 *  - No pad connected: no gamepad at all. The device is then 3537:0575 "C2
 *    receiver", the log plus a HID interface with only the vendor
 *    collection (`0F 17 55 88` still works), so no host takes it for a
 *    controller. The pad's first word (its hello, or an SInput pad's
 *    report) brings the gamepad up in its mode; its disconnect takes it
 *    down again.
 * The pad's reports (`a1 <id> <63 B>` from Bluetooth, dg_bt.c) go out on
 * the interrupt IN endpoint as `<id> <63 B>` as they arrive (or from a 2 ms
 * hardware timer if the endpoint is still full); SInput's command report 0x03 (SET_REPORT on EP0: rumble,
 * LEDs, ...) goes back to the pad (dg_bt_output).
 *
 * The SDK's USB stack calls the HID class hooks (hid_register,
 * hid_desc_config, hid_release) because TCFG_USB_SLAVE_USER_HID is set, and
 * the device descriptor / strings come from here (patch_sdk.py). */
#include "system/includes.h"
#include "app_config.h"
#include "usb/device/usb_stack.h"
#include "usb/device/hid.h"
#include "usb/device/cdc.h"
#include "usb/device/descriptor.h"
#include "usb_config.h"

#ifdef CONFIG_DONGLE_FW

#include <string.h>
#include "dongle.h"
#include "gp_log.h"
#include "gp_sinput.h"
#include "gp_xinput.h"
#include "gp_ds4.h"
#include "gp_switch.h"
#include "gamepad.h"
#include "gp_transport.h"                 /* enum gp_protocol */

#define DG_XINPUT_VID       0x3537          /* stock's receiver in Xbox mode: 3537:100B */
#define DG_XINPUT_PID       0x100B
#define DG_XINPUT_BCD       0x0121          /* stock receiver v1.21 */
#define DG_USB_VID          0x2E8A          /* SInput (SDL picks its driver by VID / PID), as the pad on USB */
#define DG_USB_PID          0x10C6
#define DG_USB_BCD          0x0100
#define DG_IDLE_VID         0x3537          /* no pad: stock's receiver ID with no pad (docs/dongle.md) */
#define DG_IDLE_PID         0x0575
#define DG_IDLE_NAME        "C2 receiver"
/* Switch: a real Pro Controller's bcdDevice is 0x0200, the pad's on its
 * cable too; the receiver says 0x0210 so tools/c2log.py tells them apart
 * (no vendor collection to tell by here). */
#define DG_SW_BCD           0x0210
#define DG_HID_MAXP         64

/* What the USB device is up as: enum gp_protocol's SInput / XInput, or
 * this (no pad connected: no gamepad, dg_usb.c's top). */
#define DG_PROTO_IDLE       0xFF

static const u8 dg_vendor_desc[] = {
    0x06, 0xF0, 0xFF,                       /* usage page 0xFFF0 (GameSir Connect's) */
    0x09, 0x40,                             /* usage 0x40 */
    0xA1, 0x01,                             /* collection (application) */
    0x15, 0x00, 0x26, 0xFF, 0x00,           /*   logical 0..255 */
    0x75, 0x08,                             /*   8 bits */
    0x85, 0x10, 0x09, 0x21, 0x95, 0x3F, 0x81, 0x02,   /* input report 0x10, 63 bytes (unused) */
    0x85, 0x0F, 0x09, 0x22, 0x95, 0x3F, 0x91, 0x02,   /* output report 0x0F, 63 bytes */
    0xC0,
};

#define DG_REPORT_DESC_MAX  320             /* SInput's or the DS4's, then the vendor collection */
static u8 dg_report_desc[DG_REPORT_DESC_MAX];
static u16 dg_report_desc_len;

static volatile u8 dg_usb_proto = DG_PROTO_IDLE;     /* what the device is up as */

/* The report descriptor of the HID interface: SInput's or the DS4's + the
 * vendor collection, or (idle) the vendor collection alone. Built in
 * hid_register for the mode. */
static const u8 *dg_report(u16 *len)
{
    if (dg_usb_proto == DG_PROTO_IDLE) {
        *len = sizeof(dg_vendor_desc);
        return dg_vendor_desc;
    }
    *len = dg_report_desc_len;
    return dg_report_desc;
}

/* The HID interface: interrupt IN, and in DS4 and Switch mode interrupt
 * OUT (the last 7 bytes; hid_desc_config leaves them out otherwise). */
#define DG_ITF_NUM_EP_AT        4
#define DG_ITF_REPORT_LEN_AT    (USB_DT_INTERFACE_SIZE + 7)   /* wDescriptorLength in the HID descriptor */
#define DG_DS4_EP_OUT           HID_EP_OUT  /* 0x02, as the pad's (gp_usb.c) */
#define DG_DS4_OUT_MAXP         64          /* the vendor report 0x0F + 63 in one (a DS4's is 64 too); the output 0x05 + 31 */
static u8 dg_itf_desc[] = {             /* the report length, the endpoints: set for the mode */
    USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE,
    0x00,                                   /* bInterfaceNumber: set in hid_desc_config */
    0x00,
    0x01,                                   /* bNumEndpoints: set in hid_desc_config */
    USB_CLASS_HID, 0x00, 0x00,
    0x00,
    0x09, USB_HID_DT_HID,
    0x11, 0x01,                             /* HID 1.11 */
    0x00, 0x01,
    0x22, 0x00, 0x00,                       /* the report descriptor's length: hid_register */
    USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
    USB_DIR_IN | HID_EP_IN, USB_ENDPOINT_XFER_INT,
    LOBYTE(DG_HID_MAXP), HIBYTE(DG_HID_MAXP),
    0x01,                                   /* 1 ms */
    /* interrupt OUT: DS4 and Switch only (64 B: a Pro Controller's too) */
    USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
    DG_DS4_EP_OUT, USB_ENDPOINT_XFER_INT,
    LOBYTE(DG_DS4_OUT_MAXP), HIBYTE(DG_DS4_OUT_MAXP),
    0x04,                                   /* 4 ms */
};

static volatile s8 dg_usb_id = -1;
static volatile u8 dg_usb_ready;            /* SInput: the host fetched the report descriptor; XInput: configured */
static volatile u8 dg_usb_mode = GP_PROTO_SINPUT;    /* the pad's mode (saved): what it is up as with a pad */
static volatile u8 dg_usb_pad;                       /* a pad is connected and has spoken */
static u8 *dg_ep_out_dma;

/* ---- The pad's reports out on the IN endpoint ---------------------------------
 * Two slots, newest wins in each: the state report (0x01, ~300/s) and any
 * other (0x02 features, answers): the other goes first. Written from the BT
 * task (dg_usb_put) and sent right away when the endpoint is free, else
 * by the 2 ms usr_timer; both with interrupts off, so one writer at a time. */
static u8 dg_slot_state[DG_HID_MAXP], dg_slot_other[DG_HID_MAXP], dg_slot_batt[DG_HID_MAXP];
static volatile u8 dg_have_state, dg_have_other, dg_have_batt;   /* other: report 2; batt: report 4 (Linux's battery) */
volatile u32 dg_usb_sent, dg_usb_dropped;
volatile struct dg_lat dg_lat;
volatile u8 dg_echo_state;
volatile u32 dg_echo_t0, dg_echo_t1, dg_echo_t2;
static u32 dg_state_t;                      /* when the state slot was filled */
static u32 dg_full_since;                   /* the IN endpoint has held a packet since (dg_us), 0: free */
volatile u32 dg_usb_flushed, dg_usb_resetups;
static u8 dg_flush_run;                     /* stale flushes in a row */
/* Any packet flushed unread makes the battery report (0x04) due again.
 * Linux reads the IN endpoint only while something has the gamepad open
 * (Steam, SDL, a hidraw reader); the battery report sent at the connect
 * went unread, counted as sent, and the next came a minute later: the
 * battery read "No data" for up to a minute after the gamepad was opened
 * (2026-11-24). Re-arming only when the battery itself was flushed, or
 * when the host seemed to start reading again, still missed now and then
 * (the endpoint looks to take a packet into a second buffer, so what was
 * collected and what flushed isn't certain). So while nothing reads, the
 * battery goes out with every status from the pad (1 s), unread; once
 * something opens the gamepad the next one is read, within a second. While
 * the host reads, nothing is flushed: once a minute and on a change. */
static u32 dg_ready_us;                     /* when the host fetched the report descriptor */
static u8 *dg_ep_in_dma;

static void dg_xinput_rx(struct usb_device_t *usb_device, u32 ep);
static void dg_ds4_rx(struct usb_device_t *usb_device, u32 ep);
static void dg_sw_rx(struct usb_device_t *usb_device, u32 ep);
static void dg_sw_output(const u8 *c, int len);

/* Switch mode's protocol, for the host on this USB (gp_switch.h). Reset
 * as a host comes (dg_usb_up, a bus reset); the pad's address from its
 * hello (dg_usb_sw_mac). */
struct sw_ctx dg_sw;

static void dg_sw_reset(void)
{
    OS_ENTER_CRITICAL();
    sw_reset(&dg_sw, 1, dg_link_serial());
    OS_EXIT_CRITICAL();
}

static void dg_ep_setup(usb_dev id)
{
    if (dg_usb_proto == GP_PROTO_XINPUT) {
        usb_g_ep_config(id, XINPUT_EP | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 0, dg_ep_in_dma, XINPUT_MAXP);
        usb_g_ep_config(id, XINPUT_EP | USB_DIR_OUT, USB_ENDPOINT_XFER_INT, 1, dg_ep_out_dma, XINPUT_MAXP);
        usb_g_set_intr_hander(id, XINPUT_EP | USB_DIR_OUT, dg_xinput_rx);
        usb_enable_ep(id, XINPUT_EP);
    } else {
        usb_g_ep_config(id, HID_EP_IN | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 0, dg_ep_in_dma, DG_HID_MAXP);
        if (dg_usb_proto == GP_PROTO_DS4 || dg_usb_proto == GP_PROTO_SWITCH) {
            usb_g_ep_config(id, DG_DS4_EP_OUT | USB_DIR_OUT, USB_ENDPOINT_XFER_INT, 1, dg_ep_out_dma, DG_DS4_OUT_MAXP);
            usb_g_set_intr_hander(id, DG_DS4_EP_OUT | USB_DIR_OUT,
                                  dg_usb_proto == GP_PROTO_SWITCH ? dg_sw_rx : dg_ds4_rx);
        }
        usb_enable_ep(id, HID_EP_IN);
    }
}

/* Nothing goes out until DG_READY_US after the host fetched the report
 * descriptor: the receiver started with the pad already streaming wrote its
 * first reports while the host was still setting the device up, and the IN
 * endpoint then never got collected (twice, 2026-11-13; flushing didn't
 * revive it). */
#define DG_READY_US         100000

/* A packet the host doesn't collect within DG_IN_STALE_US is flushed: a
 * stale report is worth nothing, and the endpoint stuck full once (receiver
 * start-up with the pad already streaming, 2026-11-13: 0 reports out until
 * a restart). Also flushes while nothing on the host has the gamepad open
 * (it then doesn't poll): harmless. */
#define DG_IN_STALE_US      50000

static int dg_usb_in_free(void)
{
    u32 csr = usb_read_txcsr(dg_usb_id, HID_EP_IN);
    if (!(csr & TXCSRP_TxPktRdy)) {
        if (dg_full_since == 0) {
            dg_flush_run = 0;                       /* collected: the endpoint works */
        }
        dg_full_since = 0;
        return 1;
    }
    u32 now = dg_us();
    if (!dg_full_since) {
        dg_full_since = now | 1;
    } else if (now - dg_full_since > DG_IN_STALE_US) {
        usb_write_txcsr(dg_usb_id, HID_EP_IN,
                        (csr & ~(TXCSRP_TxPktRdy | TXCSRP_UnderRun | TXCSRP_SentStall)) | TXCSRP_FlushFIFO);
        dg_usb_flushed++;
        dg_full_since = 0;
        sinput_battery_reset();                     /* never read: the battery due again (above) */
        if (++dg_flush_run >= 3) {
            /* Stale again and again: set the endpoint up afresh (a
             * flush alone didn't bring it back). Also while nothing on the
             * host has the gamepad open (it doesn't poll then): harmless. */
            dg_flush_run = 0;
            dg_ep_setup(dg_usb_id);
            usb_write_txcsr(dg_usb_id, HID_EP_IN, usb_read_txcsr(dg_usb_id, HID_EP_IN) | TXCSRP_ClrDataTog);
            dg_usb_resetups++;
        }
    }
    return 0;
}

/* With interrupts off (one writer): the waiting report (other first) onto a
 * free endpoint. From dg_usb_put as a report arrives (no wait while the
 * host collects them in time), else from the 2 ms tick. */
static void dg_usb_try_send(void)
{
    if (dg_usb_id < 0 || dg_usb_proto == DG_PROTO_IDLE || !dg_usb_ready || dg_us() - dg_ready_us < DG_READY_US ||
        !dg_usb_in_free()) {
        return;
    }
    int xinput = dg_usb_proto == GP_PROTO_XINPUT;
    if (xinput || dg_usb_proto == GP_PROTO_DS4) {
        dg_have_other = 0;                      /* nothing but the state in Xbox and DS4 mode */
        dg_have_batt = 0;                       /* (the DS4's battery is in its state) */
    }
    /* Switch: "other" holds a subcommand's or 0x80 command's reply
     * (dg_link.c), sent before the state, as SInput's features reply. */
    int have = dg_have_other ? 2 : dg_have_batt ? 3 : dg_have_state ? 1 : 0;
    if (!have) {
        return;
    }
    const u8 *buf = have == 2 ? dg_slot_other : have == 3 ? dg_slot_batt : dg_slot_state;
    usb_g_intr_write(dg_usb_id, HID_EP_IN, (u8 *)buf, xinput ? XINPUT_REPORT_LEN : DG_HID_MAXP);
    dg_usb_sent++;
    u32 now = dg_us();
    if (have == 1) {
        dg_have_state = 0;
        u32 w = now - dg_state_t;
        dg_lat.wait_n++;
        dg_lat.wait_sum += w;
        if (w > dg_lat.wait_max) {
            dg_lat.wait_max = w;
        }
        return;
    }
    if (have == 3) {
        dg_have_batt = 0;
        return;
    }
    dg_have_other = 0;
    if (buf[0] == SINPUT_ID_FEATURES && buf[1] == DG_PING_CMD && dg_echo_state == 3) {
        u32 out = dg_echo_t1 - dg_echo_t0, air = dg_echo_t2 - dg_echo_t1, usb = now - dg_echo_t2;
        dg_lat.echo_n++;
        dg_lat.out_sum += out;
        dg_lat.air_sum += air;
        dg_lat.usb_sum += usb;
        if (out > dg_lat.out_max) dg_lat.out_max = out;
        if (air > dg_lat.air_max) dg_lat.air_max = air;
        if (usb > dg_lat.usb_max) dg_lat.usb_max = usb;
        dg_echo_state = 0;
    }
}

static int dg_sw_id(u8 id)
{
    return id == SW_ID_STATE || id == SW_ID_SIMPLE || id == SW_ID_REPLY || id == SW_ID_USB_REPLY;
}

static void dg_usb_slot(u8 id, const u8 *data, u16 len, int send_now)
{
    int sw = dg_usb_proto == GP_PROTO_SWITCH;
    if (dg_usb_proto == DG_PROTO_IDLE || (id == DG_ID_XINPUT) != (dg_usb_proto == GP_PROTO_XINPUT) ||
        (dg_usb_proto == GP_PROTO_DS4 && id != DS4_ID_USB_STATE) || sw != dg_sw_id(id)) {
        return;                                 /* no gamepad yet, or a report for the other mode (switching) */
    }
    int other = sw ? id == SW_ID_REPLY || id == SW_ID_USB_REPLY : id == SINPUT_ID_FEATURES;
    int batt = !sw && id == SINPUT_ID_BATTERY;
    u8 *slot = other ? dg_slot_other : batt ? dg_slot_batt : dg_slot_state;
    volatile u8 *have = other ? &dg_have_other : batt ? &dg_have_batt : &dg_have_state;
    OS_ENTER_CRITICAL();
    if (*have) {
        dg_usb_dropped++;                       /* not collected yet: replaced by the newer one */
    }
    if (id == DG_ID_XINPUT) {                   /* the whole 20-byte XInput message */
        memcpy(slot, data, len < XINPUT_REPORT_LEN ? len : XINPUT_REPORT_LEN);
    } else {
        u16 n = len < DG_HID_MAXP - 1 ? len : DG_HID_MAXP - 1;
        slot[0] = id;
        memcpy(slot + 1, data, n);
        memset(slot + 1 + n, 0, DG_HID_MAXP - 1 - n);
    }
    *have = 1;
    if (!other && !batt) {
        dg_state_t = dg_us();
    }
    if (send_now) {
        dg_usb_try_send();
    }
    OS_EXIT_CRITICAL();
}

void dg_usb_put(u8 id, const u8 *data, u16 len)
{
    dg_usb_slot(id, data, len, 1);
}

/* From inside the USB interrupt's control request (SET_REPORT): only into
 * the slot; the 2 ms tick sends it. Writing the IN endpoint there, in the
 * middle of EP0's transfer, left the endpoint dead: the receiver's own
 * features reply to SDL's request (2026-11-13: 0 reports out to
 * testcontroller with the link pad; the SInput pad, whose reply comes over
 * Bluetooth, was fine). */
void dg_usb_queue(u8 id, const u8 *data, u16 len)
{
    dg_usb_slot(id, data, len, 0);
}

/* An SInput pad's report (no link): `a1 <id> <data>` as it came. Such a
 * pad has no Xbox mode here: SInput on USB. */
void dg_usb_forward(const u8 *pkt, u16 len)
{
    if (len >= 2 && pkt[0] == 0xa1 && pkt[1] == SINPUT_ID_STATE && !dg_link_active()) {
        dg_usb_set_mode(GP_MODE_STEAM);
    }
    if (len >= 2 && pkt[0] == 0xa1) {           /* HIDP DATA | INPUT, then the report ID */
        dg_usb_put(pkt[1], pkt + 2, len - 2);
    }
}

static void dg_usb_send_tick(void *priv)
{
    OS_ENTER_CRITICAL();
    dg_usb_try_send();
    OS_EXIT_CRITICAL();
}

/* ---- The bootloader command ------------------------------------------------- */
static void dg_isp_later(void *priv)        /* hardware timer interrupt: works with the app task stuck */
{
    dg_enter_bootloader();
}

/* `0F 17 55 88` (fw-re/enter_bootloader.py): on EP0 (SET_REPORT) or, in DS4
 * mode, the interrupt OUT endpoint, where Linux's hidraw writes go once the
 * interface has one. Ungated (docs/dongle.md). */
static int dg_isp_cmd(const u8 *buf, u32 len)
{
    if (len >= 4 && buf[0] == 0x0F && buf[1] == 0x17 && buf[2] == 0x55 && buf[3] == 0x88) {
        gp_log("usb: 0F 17 55 88: entering the USB bootloader");
        usr_timeout_add(NULL, dg_isp_later, 50, 1);   /* let the transfer finish */
        return 1;
    }
    return 0;
}

/* ---- DS4 mode: the host's output report -> the pad's SInput commands --------
 * Rumble -> haptics type 2 (01 02 big 0 small 0, as Xbox mode's), the
 * lightbar -> one colour on Home + grips (04 R G B): what the pad does with
 * its own DS4 output (core/gp_ds4.c). Two commands, two slots in
 * dg_bt_output: neither replaces the other. USB interrupt; dg_bt_output
 * only copies. */
static void dg_ds4_output(const u8 *c, int len)
{
    struct ds4_out o;
    if (!ds4_parse_output(c, len, &o)) {
        return;
    }
    if (o.valid & DS4_OUT_RUMBLE) {
        const u8 cmd[6] = { 0x01, 0x02, o.big, 0, o.small, 0 };
        dg_bt_output(SINPUT_ID_COMMAND, cmd, sizeof(cmd));
    }
    if (o.valid & DS4_OUT_LIGHTBAR) {
        const u8 cmd[4] = { 0x04, o.rgb[0], o.rgb[1], o.rgb[2] };
        dg_bt_output(SINPUT_ID_COMMAND, cmd, sizeof(cmd));
    }
}

static void dg_ds4_rx(struct usb_device_t *usb_device, u32 ep)
{
    u8 buf[DG_DS4_OUT_MAXP];
    u32 len = usb_g_intr_read(usb_device2id(usb_device), DG_DS4_EP_OUT, buf, sizeof(buf), 0);
    if (!dg_isp_cmd(buf, len)) {
        dg_ds4_output(buf, len);
    }
}

/* GET_REPORT: the feature reports hid-playstation reads at the connect
 * (0x12 the address, 0x02 the calibration, 0xA3 the versions; without them
 * it gives up on the pad), as the pad answers them on its cable. The
 * address: the pad's, from its hello (zeros before one). */
static void dg_ds4_get_report(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    static u8 buf[DS4_FEAT_MAX_LEN];
    int n = HIBYTE(req->wValue) == 0x03 ? ds4_feature(LOBYTE(req->wValue), 0, dg_link_serial(), buf) : 0;
    if (n) {
        usb_set_data_payload(usb_device, req, buf, n);
    } else {
        gp_log("usb: ds4 GET_REPORT %04x: none", req->wValue);
        usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
    }
}

static u32 dg_set_report(struct usb_device_t *usb_device, struct usb_ctrlrequest *setup)
{
    u8 buf[DG_HID_MAXP];
    u32 len = MIN(sizeof(buf), setup->wLength);
    usb_read_ep0(usb_device2id(usb_device), buf, len);
    if (dg_isp_cmd(buf, len)) {
        /* the bootloader, after the status stage */
    } else if (dg_usb_proto == GP_PROTO_DS4) {
        dg_ds4_output(buf, len);                        /* 0x05 (hidraw, SDL, Windows): rumble, lightbar */
    } else if (dg_usb_proto == GP_PROTO_SWITCH) {
        dg_sw_output(buf, len);                         /* hidraw without the OUT endpoint */
    } else if (len >= 2 && buf[0] == SINPUT_ID_COMMAND) {
        if (buf[1] == DG_PING_CMD) {
            dg_link_ping();                             /* a timed round trip over the air (dg_link.c) */
        } else if (buf[1] == 0x02 && dg_link_features()) {
            /* features request: answered here, from the pad's hello */
        } else {
            dg_bt_output(buf[0], buf + 1, len - 1);   /* to the pad (sent from the app task) */
        }
    }
    return USB_EP0_STAGE_SETUP;
}

/* ---- Switch mode: the host's output -> the protocol here ---------------------
 * Replies are queued for dg_link.c's next state (dg_link_sw_report); the
 * effects go to the pad as SInput commands: rumble -> haptics type 2 (01
 * 02 left 0 right 0, as DS4 mode's), player lights -> the player number
 * (03 n). USB interrupt; dg_bt_output only copies. */
static void dg_sw_output(const u8 *c, int len)
{
    struct sw_effect e;
    if (!sw_host_output(&dg_sw, c, len, &e)) {
        return;
    }
    if (e.valid & SW_FX_RUMBLE) {
        const u8 cmd[6] = { 0x01, 0x02, e.big, 0, e.small, 0 };
        dg_bt_output(SINPUT_ID_COMMAND, cmd, sizeof(cmd));
    }
    if (e.valid & SW_FX_PLAYER) {
        const u8 cmd[2] = { 0x03, e.player };
        dg_bt_output(SINPUT_ID_COMMAND, cmd, sizeof(cmd));
    }
}

static void dg_sw_rx(struct usb_device_t *usb_device, u32 ep)
{
    u8 buf[DG_DS4_OUT_MAXP];
    u32 len = usb_g_intr_read(usb_device2id(usb_device), DG_DS4_EP_OUT, buf, sizeof(buf), 0);
    if (!dg_isp_cmd(buf, len)) {
        dg_sw_output(buf, len);
    }
}

/* ---- Xbox mode: the host's OUT packets -> the pad's SInput commands ----------
 * Rumble (00 08 00 big small ..) -> haptics type 2 (01 02 La 0 Ra 0, the
 * pad's sinput_command); the LED ring (01 03 n) -> player number (03 n),
 * mapped as the pad's own xinput_led: 2..5 and 6..9 are players 1..4. USB
 * interrupt; dg_bt_output only copies. */
static void dg_xinput_rx(struct usb_device_t *usb_device, u32 ep)
{
    u8 c[XINPUT_MAXP];
    u32 len = usb_g_intr_read(usb_device2id(usb_device), XINPUT_EP, c, sizeof(c), 0);
    if (len >= 5 && c[0] == 0x00 && c[1] == 0x08) {
        const u8 cmd[6] = { 0x01, 0x02, c[3], 0, c[4], 0 };
        dg_bt_output(SINPUT_ID_COMMAND, cmd, sizeof(cmd));
    } else if (len >= 3 && c[0] == 0x01 && c[1] == 0x03) {
        u8 n = c[2];
        const u8 cmd[2] = { 0x03, n >= 2 && n <= 5 ? n - 1 : n >= 6 && n <= 9 ? n - 5 : 0 };
        dg_bt_output(SINPUT_ID_COMMAND, cmd, sizeof(cmd));
    }
}

/* ---- The gamepad interface ---------------------------------------------------- */
static void dg_ep_init(struct usb_device_t *usb_device)
{
    dg_ep_setup(usb_device2id(usb_device));
}

static void dg_itf_reset(struct usb_device_t *usb_device, u32 itf)
{
    dg_usb_ready = 0;                           /* bus reset: enumerating again */
    dg_sw_reset();                              /* a host starting over: its handshake again */
    dg_ep_init(usb_device);
}

static u32 dg_itf_handler(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    if (req == (void *)-1) {
        return 0;
    }
    u8 *tx = usb_get_setup_buffer(usb_device);
    int xinput = dg_usb_proto == GP_PROTO_XINPUT;
    switch (req->bRequestType & USB_TYPE_MASK) {
    case USB_TYPE_STANDARD:
        switch (req->bRequest) {
        case USB_REQ_GET_DESCRIPTOR:
            if (xinput) {
                usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);   /* no HID descriptors */
            } else if (HIBYTE(req->wValue) == USB_HID_DT_HID) {
                usb_set_data_payload(usb_device, req, dg_itf_desc + USB_DT_INTERFACE_SIZE, 9);
            } else if (HIBYTE(req->wValue) == USB_HID_DT_REPORT) {
                if (!dg_usb_ready) {
                    dg_ep_init(usb_device);     /* once per bus reset: not under a packet in flight */
                }
                u16 len;
                const u8 *desc = dg_report(&len);
                usb_set_data_payload(usb_device, req, (u8 *)desc, len);
                if (!dg_usb_ready) {
                    dg_ready_us = dg_us();
                    dg_usb_ready = 1;           /* a HID driver took the interface */
                    gp_log(dg_usb_proto == DG_PROTO_IDLE ? "usb: host opened the vendor interface (no pad)" :
                           "usb: host opened the gamepad");
                }
            } else {
                usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            }
            break;
        case USB_REQ_SET_INTERFACE:
            usb_set_setup_phase(usb_device, (usb_device->bDeviceStates == USB_CONFIGURED && req->wValue == 0) ?
                                USB_EP0_STAGE_SETUP : USB_EP0_SET_STALL);
            break;
        case USB_REQ_GET_INTERFACE:
            if (req->wValue || req->wLength != 1 || usb_device->bDeviceStates != USB_CONFIGURED) {
                usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            } else {
                tx[0] = 0;
                usb_set_data_payload(usb_device, req, tx, 1);
            }
            break;
        default:
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            break;
        }
        break;
    case USB_TYPE_CLASS:
        if (xinput) {
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            break;
        }
        switch (req->bRequest) {
        case USB_REQ_SET_IDLE:
            usb_set_setup_phase(usb_device, USB_EP0_STAGE_SETUP);
            break;
        case USB_REQ_GET_IDLE:
            tx[0] = 0;
            usb_set_data_payload(usb_device, req, tx, 1);
            break;
        case USB_REQ_SET_REPORT:
            usb_set_setup_recv(usb_device, dg_set_report);
            break;
        case USB_REQ_GET_REPORT:
            if (dg_usb_proto == GP_PROTO_DS4) {
                dg_ds4_get_report(usb_device, req);
            } else {
                usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            }
            break;
        default:
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            break;
        }
        break;
    default:
        usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        break;
    }
    return 0;
}

u32 hid_register(const usb_dev usb_id)
{
    static u8 timer_on;
    int ds4 = dg_usb_proto == GP_PROTO_DS4, sw = dg_usb_proto == GP_PROTO_SWITCH;
    const u8 *pad_desc = ds4 ? ds4_usb_report_desc : sw ? sw_usb_report_desc : sinput_report_desc;
    u16 pad_len = ds4 ? ds4_usb_report_desc_len : sw ? sw_usb_report_desc_len : SINPUT_REPORT_DESC_LEN;
    u16 vendor_len = sw ? 0 : sizeof(dg_vendor_desc);   /* Switch: a real one's descriptor alone */
    if (pad_len + vendor_len > sizeof(dg_report_desc)) {
        gp_log("ERROR usb: report descriptor %u B > %u", (unsigned)(pad_len + vendor_len), (unsigned)sizeof(dg_report_desc));
        pad_len = 0;
    }
    memcpy(dg_report_desc, pad_desc, pad_len);
    memcpy(dg_report_desc + pad_len, dg_vendor_desc, vendor_len);
    dg_report_desc_len = pad_len + vendor_len;
    u16 len;
    dg_report(&len);
    dg_itf_desc[DG_ITF_REPORT_LEN_AT] = LOBYTE(len);
    dg_itf_desc[DG_ITF_REPORT_LEN_AT + 1] = HIBYTE(len);
    if (!timer_on) {
        timer_on = 1;
        usr_timer_add(NULL, dg_usb_send_tick, 2, 1);   /* 2 ms: the usr_timer base */
    }
    if (dg_usb_proto == GP_PROTO_XINPUT) {
        dg_ep_in_dma = usb_alloc_ep_dmabuffer(usb_id, XINPUT_EP | USB_DIR_IN, XINPUT_MAXP);
        dg_ep_out_dma = usb_alloc_ep_dmabuffer(usb_id, XINPUT_EP, XINPUT_MAXP);
    } else {
        dg_ep_in_dma = usb_alloc_ep_dmabuffer(usb_id, HID_EP_IN | USB_DIR_IN, DG_HID_MAXP);
        if (ds4 || sw) {
            dg_ep_out_dma = usb_alloc_ep_dmabuffer(usb_id, DG_DS4_EP_OUT, DG_DS4_OUT_MAXP);   /* patch_sdk.py: pool room */
        }
    }
    return 0;
}

void hid_release(const usb_dev usb_id)
{
    dg_usb_ready = 0;
}

u32 hid_desc_config(const usb_dev usb_id, u8 *ptr, u32 *cur_itf_num)
{
    int xinput = dg_usb_proto == GP_PROTO_XINPUT;
    int out = dg_usb_proto == GP_PROTO_DS4 || dg_usb_proto == GP_PROTO_SWITCH;
    dg_itf_desc[DG_ITF_NUM_EP_AT] = out ? 2 : 1;
    u32 n = xinput ? XINPUT_ITF_DESC_LEN : sizeof(dg_itf_desc) - (out ? 0 : USB_DT_ENDPOINT_SIZE);
    memcpy(ptr, xinput ? xinput_itf_desc : dg_itf_desc, n);
    ptr[2] = *cur_itf_num;
    if (usb_set_interface_hander(usb_id, *cur_itf_num, dg_itf_handler) != *cur_itf_num ||
        usb_set_reset_hander(usb_id, *cur_itf_num, dg_itf_reset) != *cur_itf_num) {
        gp_log("ERROR usb: HID interface handlers refused");
    }
    (*cur_itf_num)++;
    return n;
}

/* The `usb` log command: the IN endpoint's state. */
void dg_usb_dump(void)
{
    gp_log("usb: id %d, ready %u (%u ms ago), IN csr %04x, slots state %u other %u, sent %u, flushed %u, re-setups %u",
           dg_usb_id, dg_usb_ready, (unsigned)((dg_us() - dg_ready_us) / 1000),
           dg_usb_id >= 0 ? (unsigned)usb_read_txcsr(dg_usb_id, HID_EP_IN) : 0, dg_have_state, dg_have_other,
           (unsigned)dg_usb_sent, (unsigned)dg_usb_flushed, (unsigned)dg_usb_resetups);
}

/* ---- Device descriptor and strings ------------------------------------------- */
void get_device_descriptor(u8 *ptr)
{
    int xinput = dg_usb_proto == GP_PROTO_XINPUT, idle = dg_usb_proto == DG_PROTO_IDLE;
    int ds4 = dg_usb_proto == GP_PROTO_DS4, sw = dg_usb_proto == GP_PROTO_SWITCH;
    u16 vid = idle ? DG_IDLE_VID : xinput ? DG_XINPUT_VID : ds4 ? DS4_VID : sw ? SW_VID : DG_USB_VID;
    u16 pid = idle ? DG_IDLE_PID : xinput ? DG_XINPUT_PID : ds4 ? DS4_PID : sw ? SW_PID : DG_USB_PID;
    u16 bcd = xinput ? DG_XINPUT_BCD : ds4 ? DS4_VERSION : sw ? DG_SW_BCD : DG_USB_BCD;
    const u8 d[USB_DT_DEVICE_SIZE] = {
        USB_DT_DEVICE_SIZE, USB_DT_DEVICE,
        0x00, 0x02,                         /* USB 2.0 */
        0xef, 0x02, 0x01,                   /* composite with interface association (CDC) */
        EP0_SETUP_LEN,
        LOBYTE(vid), HIBYTE(vid), LOBYTE(pid), HIBYTE(pid),
        LOBYTE(bcd), HIBYTE(bcd),
        0x01, 0x02, 0x03,
        0x01,
    };
    memcpy(ptr, d, sizeof(d));
}

static void dg_usb_string(u8 *ptr, const char *a)
{
    int n = 2;
    for (const char *s = a; *s && n < 64; s++, n += 2) {
        ptr[n] = *s;
        ptr[n + 1] = 0;
    }
    ptr[0] = n;
    ptr[1] = USB_DT_STRING;
}

void get_manufacture_str(u8 *ptr)
{
    dg_usb_string(ptr, dg_usb_proto == GP_PROTO_DS4 ? DS4_VENDOR_NAME :
                       dg_usb_proto == GP_PROTO_SWITCH ? SW_VENDOR_NAME : GP_VENDOR_NAME);   /* DS4, Switch: as a real one */
}

void get_product_str(u8 *ptr)
{
    dg_usb_string(ptr, dg_usb_proto == GP_PROTO_XINPUT ? XINPUT_NAME :
                       dg_usb_proto == GP_PROTO_DS4 ? DS4_NAME :
                       dg_usb_proto == GP_PROTO_SWITCH ? SW_NAME :
                       dg_usb_proto == DG_PROTO_IDLE ? DG_IDLE_NAME : GP_PRODUCT_NAME);
}

/* Xbox mode: the Microsoft OS descriptors (core/gp_xinput_desc.c), as the
 * pad's wired Xbox mode answers them (gp_usb.c). patch_sdk.py routes device
 * requests here from the SDK's user_setup_filter. */
u32 gp_usb_setup(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    if (dg_usb_proto != GP_PROTO_XINPUT) {
        return 0;
    }
    if ((req->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD && req->bRequest == USB_REQ_GET_DESCRIPTOR &&
        HIBYTE(req->wValue) == USB_DT_STRING && LOBYTE(req->wValue) == MS_OS_STRING_INDEX) {
        usb_set_data_payload(usb_device, req, xinput_ms_os_string, XINPUT_MS_OS_STRING_LEN);
        return 1;
    }
    if ((req->bRequestType & USB_TYPE_MASK) == USB_TYPE_VENDOR && req->bRequest == MS_OS_VENDOR_CODE) {
        if (req->wIndex == 0x0004) {
            usb_set_data_payload(usb_device, req, xinput_ms_compat_id, XINPUT_MS_COMPAT_ID_LEN);
        } else {
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);   /* no extended properties */
        }
        return 1;
    }
    return 0;
}

/* ---- Up and down, and the mode (app task) -------------------------------------
 * The pad's mode is saved (DG_CFG_ID): a pad that connects without a hello
 * yet (a mode-change on the way) is still shown in its last mode. The
 * pad's hello has the last word (dg_usb_set_mode). Without a pad the
 * device is the idle one (no gamepad); a change of either re-enumerates. */
#define DG_CFG_ID           49                  /* syscfg user id (apps/hid uses up to 47) */
#define DG_CFG_MAGIC        0xD6

static const char *dg_proto_name(int p)
{
    return p == DG_PROTO_IDLE ? "no pad: no gamepad (3537:0575)" :
           p == GP_PROTO_XINPUT ? "XInput gamepad (Xbox 360, 3537:100B)" :
           p == GP_PROTO_DS4 ? "DS4 gamepad (DualShock 4, 054C:09CC)" :
           p == GP_PROTO_SWITCH ? "Switch gamepad (Pro Controller, 057E:2009)" : "SInput gamepad (2E8A:10C6)";
}

void dg_usb_load_mode(void)
{
    u8 r[2] = {0};
    if (syscfg_read(DG_CFG_ID, r, sizeof(r)) == sizeof(r) && r[0] == DG_CFG_MAGIC &&
        (r[1] == GP_PROTO_SINPUT || r[1] == GP_PROTO_XINPUT || r[1] == GP_PROTO_DS4 || r[1] == GP_PROTO_SWITCH)) {
        dg_usb_mode = r[1];
    }
    gp_log("usb: saved mode %s", dg_proto_name(dg_usb_mode));
}

/* Any context (the BT task: the pad's hello, an SInput pad's report): the
 * pad is here and in this mode. */
void dg_usb_set_mode(int mode)
{
    dg_usb_mode = mode == GP_MODE_XBOX ? GP_PROTO_XINPUT : mode == GP_MODE_DS4 ? GP_PROTO_DS4 :
                  mode == GP_MODE_SWITCH ? GP_PROTO_SWITCH : GP_PROTO_SINPUT;
    dg_usb_pad = 1;
}

/* Any context (the BT task: the pad's link went down). */
void dg_usb_pad_gone(void)
{
    dg_usb_pad = 0;
}

int dg_usb_xinput(void)
{
    return dg_usb_proto == GP_PROTO_XINPUT;
}

int dg_usb_ds4(void)
{
    return dg_usb_proto == GP_PROTO_DS4;
}

int dg_usb_switch(void)
{
    return dg_usb_proto == GP_PROTO_SWITCH;
}

/* dg_link.c, BT task: the protocol's next report for this state, if any
 * (gp_switch.h: every pad state feeds the IMU history; replies first,
 * while the reply slot is free; the state at 8 ms). */
void dg_usb_switch_state(const struct gp_state *s, const struct gp_battery *b, u32 now_ms)
{
    u8 rep[SW_REPORT_MAX], id;
    OS_ENTER_CRITICAL();                        /* against the USB interrupt's resets */
    dg_sw.hold = dg_have_other;                 /* only this task fills the slot: it stays as seen */
    int n = sw_next_report(&dg_sw, s, b, now_ms, &id, rep);
    OS_EXIT_CRITICAL();
    if (n) {
        dg_usb_slot(id, rep, n, 1);             /* its own critical section */
    }
}

int dg_usb_sinput_up(void)
{
    return dg_usb_proto == GP_PROTO_SINPUT;
}

static u8 dg_usb_wanted(void)
{
    return dg_usb_pad ? dg_usb_mode : DG_PROTO_IDLE;
}

static void dg_usb_up(int id)
{
    dg_usb_proto = dg_usb_wanted();
    dg_usb_ready = 0;
    dg_have_state = dg_have_other = dg_have_batt = 0;
    sinput_battery_reset();                     /* the new host gets the battery with the next status */
    dg_sw_reset();                              /* Switch: the new host's handshake */
    gp_log("usb: host on the cable: %s + log", dg_proto_name(dg_usb_proto));
    usb_device_mode(id, HID_CLASS | CDC_CLASS);
    dg_usb_id = id;
    gp_log_usb_attach(id);
}

static void dg_usb_down(int close)
{
    int id = dg_usb_id;
    dg_usb_id = -1;
    dg_usb_ready = 0;
    gp_log_usb_attach(-1);
    usb_device_mode(id, 0);                     /* also lets go of D+: the host sees it leave */
    if (close) {
        usb_sie_close(id);
    }
}

/* Every 100 ms (dg_main.c): the device wanted (the pad's mode, or idle
 * without a pad) vs the device (re-enumerate; the mode saved), and XInput's
 * "ready" (no report descriptor fetch there: the device being configured is
 * the sign, as the pad's gp_usb_poll). */
void dg_usb_poll(void)
{
    static u8 saved = 0xff;
    u8 mode = dg_usb_mode;
    if (saved == 0xff) {
        saved = mode;
    }
    if (mode != saved) {
        u8 r[2] = { DG_CFG_MAGIC, mode };
        saved = mode;
        syscfg_write(DG_CFG_ID, r, sizeof(r));
    }
    u8 want = dg_usb_wanted();
    if (dg_usb_id < 0) {
        dg_usb_proto = want;                    /* no host: up as it when one comes */
        return;
    }
    if (want != dg_usb_proto) {
        int id = dg_usb_id;
        gp_log("usb: %s: enumerating again", want == DG_PROTO_IDLE ? "the pad is gone" :
               dg_usb_proto == DG_PROTO_IDLE ? "a pad is here" : "the pad's mode changed");
        dg_usb_down(0);
        dg_usb_up(id);
        return;
    }
    if (dg_usb_proto == GP_PROTO_XINPUT) {
        struct usb_device_t *dev = usb_id2device(dg_usb_id);
        int configured = dev && dev->bDeviceStates == USB_CONFIGURED;
        if (configured && !dg_usb_ready) {
            dg_ep_setup(dg_usb_id);
            dg_ready_us = dg_us();
            dg_usb_ready = 1;
            gp_log("usb: configured as XInput");
        } else if (!configured && dg_usb_ready) {
            dg_usb_ready = 0;
        }
    }
}

int dg_usb_event(struct sys_event *e)
{
    if ((u32)e->arg != DEVICE_EVENT_FROM_OTG) {
        return 0;
    }
    const char *msg = (const char *)e->u.dev.value;
    if (msg[0] != 's') {                    /* only device ("slave") mode */
        return 1;
    }
    if (e->u.dev.event == DEVICE_EVENT_IN) {
        dg_crumb(7);
        dg_usb_up(msg[2] - '0');
    } else if (e->u.dev.event == DEVICE_EVENT_OUT && dg_usb_id >= 0) {
        dg_usb_down(1);
        gp_log("usb: host gone");
    }
    return 1;
}

#endif
