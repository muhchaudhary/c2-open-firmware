/*
 * gp_xinput.h: the wired Xbox 360 controller protocol ("XInput"),
 * transport-free: the 20-byte input report and the host's rumble / LED
 * commands. USB only (gp_usb.c has the device side); Bluetooth stays SInput.
 *
 * Input report: 00 14, buttons (u16: d-pad up down left right, start, back,
 * L3, R3, LB, RB, guide, -, A, B, X, Y), LT, RT (u8), LX LY RX RY (s16, +Y
 * = up), 6 bytes zero. Output: 00 08 00 big small 00 00 00 (rumble),
 * 01 03 n (LED pattern).
 */
#ifndef GP_XINPUT_H
#define GP_XINPUT_H

#include "typedef.h"
#include "gamepad.h"

#define XINPUT_REPORT_LEN   20
#define XINPUT_NAME         "Xbox 360 Controller for Windows"   /* stock's (the pad wired, its receiver) */
#define XINPUT_EP           2           /* IN 0x82, OUT 0x02, as stock (C2 v3.56, 0x1E606C2) */
#define XINPUT_MAXP         32
#define MS_OS_STRING_INDEX  0xEE
#define MS_OS_VENDOR_CODE   0x90            /* as stock's "MSFT100" string */

/* gp_xinput_desc.c: the interface (bInterfaceNumber at [2] is a
 * placeholder), and the Microsoft OS descriptors Windows reads to bind its
 * Xbox driver (string 0xEE, then the extended compat ID "XUSB10"). */
extern const u8 xinput_itf_desc[], xinput_ms_os_string[], xinput_ms_compat_id[];
extern const u16 xinput_itf_desc_len, xinput_ms_os_string_len, xinput_ms_compat_id_len;
#define XINPUT_ITF_DESC_LEN         xinput_itf_desc_len
#define XINPUT_MS_OS_STRING_LEN     xinput_ms_os_string_len
#define XINPUT_MS_COMPAT_ID_LEN     xinput_ms_compat_id_len

void xinput_encode_state(u8 *rep, const struct gp_state *s);
void xinput_command(const u8 *c, int len);      /* an OUT packet from the host; any context */

#endif
