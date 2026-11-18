/*
 * gp_ds4.h: DS4 mode, a DualShock 4 (2nd revision, 054C:09CC) as PCs know
 * it: Linux's hid-playstation, SDL's PS4 driver (Steam), DS4Windows.
 * Transport-free: the report descriptors (USB, Bluetooth), the input
 * reports, the feature reports hosts read at the connect, and the host's
 * output report (rumble, lightbar). docs/ds4.md.
 *
 * Not for a PS4 console: it signs a challenge with a Sony key the pad
 * doesn't have (no auth chip on the C2, docs/stock-features.md).
 */
#ifndef GP_DS4_H
#define GP_DS4_H

#include "typedef.h"
#include "gamepad.h"

#define DS4_VID             0x054C
#define DS4_PID             0x09CC          /* DualShock 4, 2nd revision */
#define DS4_VERSION         0x0100
#define DS4_NAME            "Wireless Controller"   /* the DS4's own (USB product, Bluetooth name) */
#define DS4_VENDOR_NAME     "Sony Interactive Entertainment"   /* its USB manufacturer: hosts name it by both */

/* Input reports, after the report ID. */
#define DS4_ID_USB_STATE    0x01
#define DS4_USB_REPORT_LEN  63              /* 64 B with the ID (hid-playstation checks the size) */
#define DS4_ID_BT_STATE     0x11
#define DS4_BT_REPORT_LEN   77              /* 78 B with the ID, the last 4 a CRC-32 */

/* Output reports (rumble, lightbar), after the report ID. */
#define DS4_ID_USB_OUTPUT   0x05            /* 32 B with the ID */
#define DS4_ID_BT_OUTPUT    0x11            /* 78 B with the ID */

/* Feature reports hosts read (GET_REPORT). */
#define DS4_FEAT_CAL_USB    0x02            /* IMU calibration, 37 B with the ID */
#define DS4_FEAT_CAL_BT     0x05            /* the same over Bluetooth, 41 B with a CRC-32 */
#define DS4_FEAT_PAIRING    0x12            /* the pad's Bluetooth address, 16 B (USB) */
#define DS4_FEAT_FIRMWARE   0xA3            /* build date, hardware and firmware version, 49 B */
#define DS4_FEAT_MAX_LEN    49

/* gp_ds4_desc.c: the descriptors. */
extern const u8 ds4_usb_report_desc[];
extern const u8 ds4_bt_report_desc[];
extern const u16 ds4_usb_report_desc_len, ds4_bt_report_desc_len;

/* The state. ts_us: the IMU timestamp (gp_core's, real time); counter: the
 * report counter (6 bits used). Bluetooth's has its CRC filled in. */
void ds4_encode_usb(u8 *rep, const struct gp_state *s, const struct gp_battery *b, u32 ts_us, u8 counter);
void ds4_encode_bt(u8 *rep, const struct gp_state *s, const struct gp_battery *b, u32 ts_us, u8 counter);

/* A feature report, ID first, into out (DS4_FEAT_MAX_LEN): its length, 0
 * if the pad has no such report. bt: the Bluetooth set (calibration with a
 * CRC-32). mac: the pad's Bluetooth address, MSB first. */
int ds4_feature(u8 id, int bt, const u8 *mac, u8 *out);

/* An output report from the host, ID first: what it sets. */
#define DS4_OUT_RUMBLE      0x01
#define DS4_OUT_LIGHTBAR    0x02
struct ds4_out {
    u8 valid;                       /* DS4_OUT_* */
    u8 big, small;                  /* motors, 0..255 (the DS4's left / strong, right / weak) */
    u8 rgb[3];
};
int  ds4_parse_output(const u8 *c, int len, struct ds4_out *o);   /* 0: not an output report */
void ds4_output(const u8 *c, int len);          /* gp_ds4.c: parse and apply; any context */

/* CRC-32 (IEEE, as zlib) of a seed byte, then data: the DS4's Bluetooth
 * reports carry it (seed 0xA1 input, 0xA2 output, 0xA3 feature). */
#define DS4_CRC_INPUT       0xA1
#define DS4_CRC_OUTPUT      0xA2
#define DS4_CRC_FEATURE     0xA3
u32 ds4_crc32(u8 seed, const u8 *data, int len);

#endif
