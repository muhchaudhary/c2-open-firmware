/* gp_usb.c: see gp_usb.h.
 *
 * The SDK's USB device stack builds the configuration from class hooks
 * (apps/common/device/usb/device/usb_device.c): with HID_CLASS it calls
 * hid_register / hid_desc_config / hid_release, which the SDK leaves to the
 * app when TCFG_USB_SLAVE_USER_HID is set (as its standard_keyboard example
 * does); these are ours, and they put up the gamepad interface of the
 * input mode (gp_settings.mode):
 *
 *  - SInput: a HID interface with one interrupt IN endpoint (64-byte
 *    packets: report ID + 63 bytes); the host's output reports come as
 *    SET_REPORT on the control endpoint (hidraw / Windows do that when there
 *    is no OUT endpoint), which keeps within the SDK's USB DMA pool.
 *  - XInput: a wired Xbox 360 controller's interface as stock's (vendor
 *    class FF / 5D / 01 with the undocumented 0x21 descriptor), interrupt
 *    IN and OUT on endpoint 2, 32 bytes each, under stock's IDs (below).
 *  - DS4: a HID interface as SInput's, with the DualShock 4's report
 *    descriptor; its feature reports answer GET_REPORT (gp_ds4.h).
 *
 * The device descriptor and strings come from here too (patch_sdk.py
 * renames the SDK's fixed ones), so the IDs follow the mode: SInput's (SDL
 * picks its driver by VID/PID), the 360 pad's, the DS4's (hid-playstation
 * and SDL match 054C:09CC), or the debug log's pid.codes test ID 1209:0001
 * when it is alone (charging mode). */
#include "system/includes.h"
#include "app_config.h"
#include "usb/device/usb_stack.h"
#include "usb/device/hid.h"
#include "usb/device/cdc.h"
#include "usb/device/descriptor.h"
#include "usb_config.h"
#include "gamepad.h"
#include "gp_usb.h"
#include "gp_sinput.h"
#include "gp_xinput.h"
#include "gp_ds4.h"
#include "gp_settings.h"
#include "gp_log.h"

#define GP_USB_MAXP         64          /* SInput: report ID + 63 bytes */
#define GP_USB_LOG_VID      0x1209      /* pid.codes test ID: the debug log alone */
#define GP_USB_LOG_PID      0x0001
/* Xbox mode as stock: GameSir's VID with the C2 (hall triggers) wired
 * PID (stock v3.56's device table, 0x1E5D07E; GameSir Connect lists it as
 * the pad's wired ID) and the name stock's Xbox mode gives (its dongle,
 * 3537:100B: "Xbox 360 Controller for Windows"). Windows binds its Xbox
 * driver to a non-Microsoft ID through Microsoft OS descriptors, as stock
 * (gp_usb_setup); Linux' xpad takes any 0x3537 XInput interface. */
#define XINPUT_VID          0x3537
#define XINPUT_PID          0x1053
#define XINPUT_BCD          0x0356          /* stock's: its firmware version */
/* XINPUT_NAME, the interface and the Microsoft OS descriptors: gp_xinput.h */

enum { USB_OFF, USB_GAMEPAD, USB_LOG_ONLY };

static volatile s8 gp_usb_id = -1;          /* the USB device while a host is on the cable */
static u8 gp_usb_mode;                      /* what it enumerates as */
static u8 gp_usb_proto;                     /* the gamepad's protocol while USB_GAMEPAD */
static u8 gp_usb_log_wanted;                /* charging mode: the log alone */
static u8 gp_usb_input;                     /* USB input on (Home + Capture tapped): the gamepad on USB */
static volatile u8 gp_usb_ready;            /* the host has the gamepad (see gp_usb_poll) */
static u8 *gp_ep_in_dma, *gp_ep_out_dma;

/* SInput's and DS4's HID interface; hid_desc_config sets it for the mode:
 * the report descriptor's length, and DS4's interrupt OUT endpoint (the
 * last 7 bytes; SInput has none). A DS4's host commands come there:
 * hid-playstation sends rumble and lightbar only to an OUT endpoint
 * (hidraw, SDL, Windows fall back to SET_REPORT, which works too). */
#define HID_ITF_NUM_EP_AT   4
#define HID_ITF_DESC_LEN_AT (USB_DT_INTERFACE_SIZE + 7)
#define DS4_EP_OUT          HID_EP_OUT      /* 0x02, as XInput's */
#define DS4_OUT_MAXP        32              /* the output report, 0x05 + 31 */
static u8 gp_hid_itf_desc[] = {
    USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE,
    0x00,                                   /* bInterfaceNumber: set in hid_desc_config */
    0x00,                                   /* bAlternateSetting */
    0x01,                                   /* bNumEndpoints: set in hid_desc_config */
    USB_CLASS_HID, 0x00, 0x00,              /* no boot interface */
    0x00,                                   /* iInterface */
    /* HID descriptor */
    0x09, USB_HID_DT_HID,
    0x11, 0x01,                             /* bcdHID 1.11 */
    0x00,                                   /* bCountryCode */
    0x01,                                   /* bNumDescriptors */
    0x22, LOBYTE(SINPUT_REPORT_DESC_LEN), HIBYTE(SINPUT_REPORT_DESC_LEN),
    /* interrupt IN */
    USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
    USB_DIR_IN | HID_EP_IN, USB_ENDPOINT_XFER_INT,
    LOBYTE(GP_USB_MAXP), HIBYTE(GP_USB_MAXP),
    0x01,                                   /* bInterval: 1 ms */
    /* interrupt OUT: DS4 only */
    USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
    DS4_EP_OUT, USB_ENDPOINT_XFER_INT,
    LOBYTE(DS4_OUT_MAXP), HIBYTE(DS4_OUT_MAXP),
    0x04,                                   /* bInterval: 4 ms */
};

/* ---- Endpoints -------------------------------------------------------------- */
static void gp_xinput_rx(struct usb_device_t *usb_device, u32 ep)
{
    u8 buf[XINPUT_MAXP];
    u32 len = usb_g_intr_read(usb_device2id(usb_device), XINPUT_EP, buf, sizeof(buf), 0);
    xinput_command(buf, len);
}

static void gp_ds4_rx(struct usb_device_t *usb_device, u32 ep)
{
    u8 buf[DS4_OUT_MAXP];
    u32 len = usb_g_intr_read(usb_device2id(usb_device), DS4_EP_OUT, buf, sizeof(buf), 0);
    ds4_output(buf, len);
}

static void gp_ep_init(struct usb_device_t *usb_device)
{
    const usb_dev id = usb_device2id(usb_device);
    if (gp_usb_proto == GP_PROTO_XINPUT) {
        usb_g_ep_config(id, XINPUT_EP | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 0, gp_ep_in_dma, XINPUT_MAXP);
        usb_g_ep_config(id, XINPUT_EP | USB_DIR_OUT, USB_ENDPOINT_XFER_INT, 1, gp_ep_out_dma, XINPUT_MAXP);
        usb_g_set_intr_hander(id, XINPUT_EP | USB_DIR_OUT, gp_xinput_rx);
        usb_enable_ep(id, XINPUT_EP);
    } else {
        usb_g_ep_config(id, HID_EP_IN | USB_DIR_IN, USB_ENDPOINT_XFER_INT, 0, gp_ep_in_dma, GP_USB_MAXP);
        if (gp_usb_proto == GP_PROTO_DS4) {
            usb_g_ep_config(id, DS4_EP_OUT | USB_DIR_OUT, USB_ENDPOINT_XFER_INT, 1, gp_ep_out_dma, DS4_OUT_MAXP);
            usb_g_set_intr_hander(id, DS4_EP_OUT | USB_DIR_OUT, gp_ds4_rx);
        }
        usb_enable_ep(id, HID_EP_IN);
    }
}

static void gp_itf_reset(struct usb_device_t *usb_device, u32 itf)
{
    gp_usb_ready = 0;                                    /* bus reset: enumerating again */
    gp_ep_init(usb_device);
}

/* SET_REPORT data stage (SInput, DS4): an output report, ID first. */
static u32 gp_hid_set_report(struct usb_device_t *usb_device, struct usb_ctrlrequest *setup)
{
    u8 buf[GP_USB_MAXP];
    u32 len = MIN(sizeof(buf), setup->wLength);
    usb_read_ep0(usb_device2id(usb_device), buf, len);
    if (gp_usb_proto == GP_PROTO_DS4) {
        ds4_output(buf, len);                            /* 0x05: rumble, lightbar */
    } else if (len >= 2 && buf[0] == SINPUT_ID_COMMAND) {
        sinput_command(buf + 1, len - 1);
    }
    return USB_EP0_STAGE_SETUP;
}

/* GET_REPORT (DS4): a feature report the host reads at the connect
 * (hid-playstation: 0x12 the address, 0x02 the calibration, 0xA3 the
 * versions; without them it gives up on the pad). */
static void gp_ds4_get_report(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    static u8 buf[DS4_FEAT_MAX_LEN];
    extern const u8 *bt_get_mac_addr(void);
    const u8 *mac = bt_get_mac_addr();
    u8 msb[6];
    for (int k = 0; k < 6; k++) {
        msb[k] = mac[5 - k];
    }
    int n = HIBYTE(req->wValue) == 0x03 ? ds4_feature(LOBYTE(req->wValue), 0, msb, buf) : 0;
    if (n) {
        usb_set_data_payload(usb_device, req, buf, n);
    } else {
        gp_log("usb: ds4 GET_REPORT %04x: none", req->wValue);
        usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
    }
}

static u32 gp_itf_handler(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    if (req == (void *)-1) {
        return 0;
    }
    u8 *tx = usb_get_setup_buffer(usb_device);
    int hid = gp_usb_proto == GP_PROTO_SINPUT || gp_usb_proto == GP_PROTO_DS4;
    int ds4 = gp_usb_proto == GP_PROTO_DS4;
    switch (req->bRequestType & USB_TYPE_MASK) {
    case USB_TYPE_STANDARD:
        switch (req->bRequest) {
        case USB_REQ_GET_DESCRIPTOR:
            if (hid && HIBYTE(req->wValue) == USB_HID_DT_HID) {
                usb_set_data_payload(usb_device, req, gp_hid_itf_desc + USB_DT_INTERFACE_SIZE, 9);
            } else if (hid && HIBYTE(req->wValue) == USB_HID_DT_REPORT) {
                gp_ep_init(usb_device);
                if (ds4) {
                    usb_set_data_payload(usb_device, req, ds4_usb_report_desc, ds4_usb_report_desc_len);
                } else {
                    usb_set_data_payload(usb_device, req, sinput_report_desc, SINPUT_REPORT_DESC_LEN);
                }
                if (!gp_usb_ready) {
                    gp_usb_ready = 1;                    /* SInput, DS4: a HID driver took the pad */
                    gp_log("usb: host opened the gamepad");
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
        if (!hid) {
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
            usb_set_setup_recv(usb_device, gp_hid_set_report);
            break;
        case USB_REQ_GET_REPORT:
            if (ds4) {
                gp_ds4_get_report(usb_device, req);
            } else {
                usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            }
            break;
        default:
            usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
            break;
        }
        break;
    default:                                             /* vendor requests (XInput hosts may ask): not supported */
        usb_set_setup_phase(usb_device, USB_EP0_SET_STALL);
        break;
    }
    return 0;
}

/* ---- The HID class hooks (usb_device.c) ----------------------------------- */
u32 hid_register(const usb_dev usb_id)
{
    if (gp_usb_proto == GP_PROTO_XINPUT) {
        gp_ep_in_dma = usb_alloc_ep_dmabuffer(usb_id, XINPUT_EP | USB_DIR_IN, XINPUT_MAXP);
        gp_ep_out_dma = usb_alloc_ep_dmabuffer(usb_id, XINPUT_EP, XINPUT_MAXP);
    } else {
        gp_ep_in_dma = usb_alloc_ep_dmabuffer(usb_id, HID_EP_IN | USB_DIR_IN, GP_USB_MAXP);
        if (gp_usb_proto == GP_PROTO_DS4) {
            gp_ep_out_dma = usb_alloc_ep_dmabuffer(usb_id, DS4_EP_OUT, DS4_OUT_MAXP);   /* patch_sdk.py: pool room */
        }
    }
    return 0;
}

void hid_release(const usb_dev usb_id)
{
    gp_usb_ready = 0;
}

u32 hid_desc_config(const usb_dev usb_id, u8 *ptr, u32 *cur_itf_num)
{
    int ds4 = gp_usb_proto == GP_PROTO_DS4;
    u16 rlen = ds4 ? ds4_usb_report_desc_len : SINPUT_REPORT_DESC_LEN;
    gp_hid_itf_desc[HID_ITF_NUM_EP_AT] = ds4 ? 2 : 1;
    gp_hid_itf_desc[HID_ITF_DESC_LEN_AT] = LOBYTE(rlen);
    gp_hid_itf_desc[HID_ITF_DESC_LEN_AT + 1] = HIBYTE(rlen);
    const u8 *d = gp_usb_proto == GP_PROTO_XINPUT ? xinput_itf_desc : gp_hid_itf_desc;
    u32 n = gp_usb_proto == GP_PROTO_XINPUT ? XINPUT_ITF_DESC_LEN :
            sizeof(gp_hid_itf_desc) - (ds4 ? 0 : USB_DT_ENDPOINT_SIZE);
    memcpy(ptr, d, n);
    ptr[2] = *cur_itf_num;
    if (usb_set_interface_hander(usb_id, *cur_itf_num, gp_itf_handler) != *cur_itf_num ||
        usb_set_reset_hander(usb_id, *cur_itf_num, gp_itf_reset) != *cur_itf_num) {
        gp_log("ERROR usb: gamepad interface handlers refused");
    }
    (*cur_itf_num)++;
    return n;
}

/* ---- Device descriptor and strings ------------------------------------------ */
void get_device_descriptor(u8 *ptr)
{
    int gamepad = gp_usb_mode == USB_GAMEPAD, xinput = gamepad && gp_usb_proto == GP_PROTO_XINPUT;
    int ds4 = gamepad && gp_usb_proto == GP_PROTO_DS4;
    u16 vid = xinput ? XINPUT_VID : ds4 ? DS4_VID : gamepad ? GP_PNP_VID : GP_USB_LOG_VID;
    u16 pid = xinput ? XINPUT_PID : ds4 ? DS4_PID : gamepad ? GP_PNP_PID : GP_USB_LOG_PID;
    /* Gamepad + log: composite with interface association (the CDC log has
     * one), class EF / 02 / 01, so Windows binds each function on its own
     * (XInput's through the OS descriptors). */
    u8 cls = gamepad ? 0xef : 0x00;
    u8 sub = gamepad ? 0x02 : 0x00;
    u8 pro = gamepad ? 0x01 : 0x00;
    u16 bcd = xinput ? XINPUT_BCD : ds4 ? DS4_VERSION : GP_PNP_VERSION;
    const u8 d[USB_DT_DEVICE_SIZE] = {
        USB_DT_DEVICE_SIZE, USB_DT_DEVICE,
        /* USB 2.0 (a full-speed device may say so), as every stock
         * descriptor: Windows asks for the Microsoft OS descriptors only
         * from 2.0 devices (with 1.1, Xbox mode had no driver) */
        0x00, 0x02,
        cls, sub, pro,
        EP0_SETUP_LEN,
        LOBYTE(vid), HIBYTE(vid), LOBYTE(pid), HIBYTE(pid),
        LOBYTE(bcd), HIBYTE(bcd),
        0x01, 0x02, 0x03,                                /* manufacturer, product, serial strings */
        0x01,                                            /* one configuration */
    };
    memcpy(ptr, d, sizeof(d));
}

/* ASCII -> a USB string descriptor, at most 31 characters (64 bytes: one
 * control packet; "Xbox 360 Controller for Windows" is exactly 31). */
static void gp_usb_string(u8 *ptr, const char *a, const char *b)
{
    int n = 2;
    for (const char *s = a; s && *s && n < 64; s++, n += 2) {
        ptr[n] = *s;
        ptr[n + 1] = 0;
    }
    for (const char *s = b; s && *s && n < 64; s++, n += 2) {
        ptr[n] = *s;
        ptr[n + 1] = 0;
    }
    ptr[0] = n;
    ptr[1] = USB_DT_STRING;
}

void get_manufacture_str(u8 *ptr)
{
    int ds4 = gp_usb_mode == USB_GAMEPAD && gp_usb_proto == GP_PROTO_DS4;
    gp_usb_string(ptr, ds4 ? DS4_VENDOR_NAME : GP_VENDOR_NAME, NULL);   /* DS4: named as a real one */
}

void get_product_str(u8 *ptr)
{
    if (gp_usb_mode == USB_GAMEPAD && gp_usb_proto == GP_PROTO_XINPUT) {
        gp_usb_string(ptr, XINPUT_NAME, NULL);
    } else if (gp_usb_mode == USB_GAMEPAD && gp_usb_proto == GP_PROTO_DS4) {
        gp_usb_string(ptr, DS4_NAME, NULL);
    } else {
        gp_usb_string(ptr, GP_PRODUCT_NAME, gp_usb_mode != USB_GAMEPAD ? " debug log" : NULL);
    }
}

/* Microsoft OS descriptors (version 1.0), XInput only, byte for byte
 * stock's (v3.56 0x1E5D06C "MSFT100", 0x1E60824 the compat ID record):
 * Windows asks for string 0xEE, then with its vendor code for the
 * "extended compat ID" feature (wIndex 4), which names XUSB10 for
 * interface 0: its Xbox driver then takes that interface. patch_sdk.py
 * routes device requests here from the SDK's user_setup_filter. */
u32 gp_usb_setup(struct usb_device_t *usb_device, struct usb_ctrlrequest *req)
{
    if (gp_usb_mode != USB_GAMEPAD || gp_usb_proto != GP_PROTO_XINPUT) {
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

/* ---- The transport ------------------------------------------------------- */
static int gp_usb_connected(void)
{
    return gp_usb_ready && gp_usb_mode == USB_GAMEPAD && gp_usb_id >= 0;
}

/* The SDK's endpoint write waits while the last packet hasn't gone out:
 * check first, so the tick never waits. */
static int gp_usb_busy(void)
{
    u32 ep = gp_usb_proto == GP_PROTO_XINPUT ? XINPUT_EP : HID_EP_IN;
    return gp_usb_id < 0 || (usb_read_txcsr(gp_usb_id, ep) & TXCSRP_TxPktRdy);
}

static int gp_usb_protocol(void)
{
    return gp_usb_proto;
}

static void gp_usb_post(u8 id, const u8 *report, int len)
{
    if (gp_usb_proto == GP_PROTO_XINPUT) {
        usb_g_intr_write(gp_usb_id, XINPUT_EP, report, len);
        return;
    }
    u8 buf[GP_USB_MAXP];
    buf[0] = id;
    memcpy(buf + 1, report, MIN(len, sizeof(buf) - 1));
    usb_g_intr_write(gp_usb_id, HID_EP_IN, buf, sizeof(buf));
}

const struct gp_transport gp_usb_transport = {
    .name = "usb",
    .connected = gp_usb_connected,
    .busy = gp_usb_busy,
    .protocol = gp_usb_protocol,
    .post = gp_usb_post,
};

int gp_usb_host_ready(void)
{
    return gp_usb_connected();
}

/* ---- Bringing the device up and down (app task) -------------------------- */
static const char *gp_proto_name(int p)
{
    return p == GP_PROTO_XINPUT ? "XInput" : p == GP_PROTO_DS4 ? "DS4" : "SInput";
}

static void gp_usb_up(int id)
{
    gp_usb_ready = 0;
    gp_usb_mode = (gp_usb_log_wanted || !gp_usb_input) ? USB_LOG_ONLY : USB_GAMEPAD;
    gp_usb_proto = gp_settings.mode == GP_MODE_XBOX ? GP_PROTO_XINPUT :
                   gp_settings.mode == GP_MODE_DS4 ? GP_PROTO_DS4 : GP_PROTO_SINPUT;
    if (gp_usb_mode == USB_GAMEPAD) {
        gp_log("usb: host on the cable, up as %s gamepad + log", gp_proto_name(gp_usb_proto));
    } else {
        gp_log("usb: host on the cable, up as the log alone%s",
               gp_usb_log_wanted ? "" : " (Home + Capture: the gamepad on USB)");
    }
    usb_device_mode(id, gp_usb_mode == USB_GAMEPAD ? (HID_CLASS | CDC_CLASS) : CDC_CLASS);
    gp_usb_id = id;
    gp_log_usb_attach(id);
}

static void gp_usb_down(int close)
{
    int id = gp_usb_id;
    gp_usb_ready = 0;
    gp_usb_id = -1;
    gp_log_usb_attach(-1);
    usb_device_mode(id, 0);                              /* also lets go of D+: the host sees it leave */
    if (close) {
        usb_sie_close(id);
    }
    gp_usb_mode = USB_OFF;
}

static void gp_usb_reenumerate(void)
{
    if (gp_usb_id >= 0) {
        int id = gp_usb_id;
        gp_usb_down(0);
        gp_usb_up(id);
    }
}

int gp_usb_event(struct sys_event *e)
{
    if (e->type != SYS_DEVICE_EVENT || (u32)e->arg != DEVICE_EVENT_FROM_OTG) {
        return 0;
    }
    const char *msg = (const char *)e->u.dev.value;
    if (msg[0] != 's') {                                 /* only device ("slave") mode */
        return 1;
    }
    if (e->u.dev.event == DEVICE_EVENT_IN) {
        gp_usb_up(msg[2] - '0');
    } else if (e->u.dev.event == DEVICE_EVENT_OUT && gp_usb_id >= 0) {
        gp_usb_down(1);
        gp_usb_input = 0;                                /* each cable starts on Bluetooth */
        gp_log("usb: host gone");
    }
    return 1;
}

int gp_usb_host_present(void)
{
    return gp_usb_id >= 0;
}

void gp_usb_set_input(int on)
{
    if (gp_usb_log_wanted || gp_usb_id < 0 || on == gp_usb_input) {
        return;
    }
    gp_usb_input = on;
    gp_log("usb: input %s", on ? "on: the gamepad on USB" : "off: back to Bluetooth");
    gp_usb_reenumerate();
}

int gp_usb_input_on(void)
{
    return gp_usb_input;
}

void gp_usb_log_only(void)
{
    gp_usb_log_wanted = 1;
    if (gp_usb_mode == USB_GAMEPAD) {
        gp_usb_reenumerate();                            /* enumerates again as the log alone */
    }
}

/* XInput has no report descriptor fetch to tell that a driver took the
 * pad: the device being configured is the sign. Its endpoints are set up
 * again then, as the SDK's CDC class does after configuration. */
void gp_usb_poll(void)
{
    if (gp_usb_id < 0 || gp_usb_mode != USB_GAMEPAD) {
        return;
    }
    struct usb_device_t *dev = usb_id2device(gp_usb_id);
    int configured = dev && dev->bDeviceStates == USB_CONFIGURED;
    if (gp_usb_proto != GP_PROTO_XINPUT) {
        return;                                          /* HID: ready when the report descriptor is fetched */
    }
    if (configured && !gp_usb_ready) {
        gp_ep_init(dev);
        gp_usb_ready = 1;
        gp_log("usb: configured as XInput");
    } else if (!configured && gp_usb_ready) {
        gp_usb_ready = 0;
    }
}
