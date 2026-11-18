/* gp_xinput_desc.c: the wired Xbox 360 (XInput) state encoder and USB
 * descriptors, on their own so the C2 receiver (boards/dongle) uses the
 * same bytes as the pad's wired Xbox mode (gp_usb.c). */
#include "system/includes.h"
#include "usb/device/usb_stack.h"
#include "usb/device/descriptor.h"
#include "usb_config.h"
#include "gp_xinput.h"

/* gp_state button -> XInput button bit. Paddles and Capture have no XInput
 * equivalent (remapping them is for the configuration app). */
static const u8 xinput_map[][2] = {
    { GP_BTN_UP, 0 }, { GP_BTN_DOWN, 1 }, { GP_BTN_LEFT, 2 }, { GP_BTN_RIGHT, 3 },
    { GP_BTN_START, 4 }, { GP_BTN_BACK, 5 }, { GP_BTN_L3, 6 }, { GP_BTN_R3, 7 },
    { GP_BTN_L1, 8 }, { GP_BTN_R1, 9 }, { GP_BTN_GUIDE, 10 },
    { GP_BTN_SOUTH, 12 }, { GP_BTN_EAST, 13 }, { GP_BTN_WEST, 14 }, { GP_BTN_NORTH, 15 },
};

static void put16(u8 *p, int v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

/* SInput's +Y is down, XInput's up. The axes are -32767..32767, so the
 * negation can't overflow. */
static int flip(int v)
{
    return v == -32768 ? 32767 : -v;
}

void xinput_encode_state(u8 *rep, const struct gp_state *s)
{
    u16 b = 0;
    for (int i = 0; i < ARRAY_SIZE(xinput_map); i++) {
        if (s->buttons & GP_BIT(xinput_map[i][0])) {
            b |= 1u << xinput_map[i][1];
        }
    }
    memset(rep, 0, XINPUT_REPORT_LEN);
    rep[0] = 0x00;                                       /* message type: input */
    rep[1] = XINPUT_REPORT_LEN;
    put16(&rep[2], b);
    rep[4] = (u16)(s->lt + 32768) >> 8;                  /* -32768..32767 -> 0..255 */
    rep[5] = (u16)(s->rt + 32768) >> 8;
    put16(&rep[6], s->lx);
    put16(&rep[8], flip(s->ly));
    put16(&rep[10], s->rx);
    put16(&rep[12], flip(s->ry));
}


const u8 xinput_itf_desc[] = {
    USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE,
    0x00,                                   /* bInterfaceNumber: set in hid_desc_config */
    0x00,
    0x02,                                   /* IN + OUT */
    0xff, 0x5d, 0x01,                       /* vendor: XInput gamepad */
    0x00,
    /* the XInput class descriptor (undocumented: its endpoints and report
     * sizes), byte for byte stock's 16-byte form */
    0x10, 0x21, 0x10, 0x01, 0x01, 0x24,
    USB_DIR_IN | XINPUT_EP, 0x14, 0x03, 0x00, 0x03, 0x13,
    XINPUT_EP, 0x00, 0x03, 0x00,
    USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
    USB_DIR_IN | XINPUT_EP, USB_ENDPOINT_XFER_INT,
    LOBYTE(XINPUT_MAXP), HIBYTE(XINPUT_MAXP),
    0x01,                                   /* 1 ms, as stock */
    USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
    XINPUT_EP, USB_ENDPOINT_XFER_INT,
    LOBYTE(XINPUT_MAXP), HIBYTE(XINPUT_MAXP),
    0x08,
};
const u16 xinput_itf_desc_len = sizeof(xinput_itf_desc);

const u8 xinput_ms_os_string[] = {
    0x12, USB_DT_STRING, 'M', 0, 'S', 0, 'F', 0, 'T', 0, '1', 0, '0', 0, '0', 0, MS_OS_VENDOR_CODE, 0x00,
};
const u16 xinput_ms_os_string_len = sizeof(xinput_ms_os_string);
const u8 xinput_ms_compat_id[] = {
    0x28, 0x00, 0x00, 0x00,                              /* dwLength 40 */
    0x00, 0x01,                                          /* bcdVersion 1.00 */
    0x04, 0x00,                                          /* wIndex: extended compat ID */
    0x01,                                                /* one function */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,                                                /* interface 0 (the gamepad) */
    0x01,
    'X', 'U', 'S', 'B', '1', '0', 0x00, 0x00,            /* compatible ID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      /* sub-compatible ID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
const u16 xinput_ms_compat_id_len = sizeof(xinput_ms_compat_id);
