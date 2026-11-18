/*
 * gp_switch.h: Switch mode, a Nintendo Switch Pro Controller (057E:2009)
 * over Bluetooth and USB: for the Switch itself (Switch 1 and 2), Linux's
 * hid-nintendo, SDL's Switch driver (Steam). Transport-free: the report
 * descriptors, the protocol (the host's subcommands, the USB handshake),
 * the input reports. docs/switch.md.
 *
 * Unlike DS4 mode there is no key to sign anything: a Pro Controller
 * needs no security chip, so the console takes it. What it does read is
 * the controller's SPI flash (calibration, colours, stick parameters):
 * the pad answers from a copy of stock's emulated page (gp_switch_desc.c).
 */
#ifndef GP_SWITCH_H
#define GP_SWITCH_H

#include "typedef.h"
#include "gamepad.h"

#define SW_VID              0x057E
#define SW_PID              0x2009          /* Pro Controller */
#define SW_VERSION          0x0200          /* bcdDevice of a real one */
#define SW_NAME             "Pro Controller"
#define SW_VENDOR_NAME      "Nintendo Co., Ltd."

/* Input reports (pad -> host). */
#define SW_ID_REPLY         0x21            /* a subcommand's reply, with the state */
#define SW_ID_STATE         0x30            /* the full state: buttons, sticks, 3 IMU samples */
#define SW_ID_SIMPLE        0x3F            /* plain HID, before the host picks 0x30 (Bluetooth) */
#define SW_ID_USB_REPLY     0x81            /* a USB command's reply */
/* Output reports (host -> pad). */
#define SW_ID_RUMBLE_SUB    0x01            /* rumble + a subcommand */
#define SW_ID_RUMBLE        0x10            /* rumble alone */
#define SW_ID_USB_CMD       0x80            /* USB: handshake, status, ... */

/* Report lengths after the ID. Bluetooth sends 0x21 / 0x30 as 49 bytes
 * (the reply's CRC-8 for subcommand 0x21 sits in the last one), 0x3F as
 * its 11; USB pads every report to the 64-byte packet. */
#define SW_BT_LEN           49
#define SW_SIMPLE_LEN       11
#define SW_USB_LEN          63
#define SW_REPORT_MAX       63

/* gp_switch_desc.c: the descriptors (as stock's, which are a real one's). */
extern const u8 sw_usb_report_desc[];
extern const u8 sw_bt_report_desc[];
extern const u16 sw_usb_report_desc_len, sw_bt_report_desc_len;

/* What the host's output asks the pad to do (sw_host_output). */
#define SW_FX_RUMBLE        0x01
#define SW_FX_PLAYER        0x02
struct sw_effect {
    u8 valid;                   /* SW_FX_* */
    u8 big, small;              /* rumble 0..255: the big (low) / small (high) motor (sw_rumble_motors) */
    u8 player;                  /* 1..4, 0 = none (from the player-light pattern) */
};

/* The protocol's state, one per transport in use. The host's output (BT
 * task, USB interrupt) is the only writer of the reply ring's head, the
 * tick (sw_next_report) the only one of its tail. */
#define SW_REPLIES          4
struct sw_reply {
    u8 id;                      /* SW_ID_REPLY or SW_ID_USB_REPLY */
    u8 len;                     /* bytes in data */
    u8 data[37];                /* 0x21: ack, subcommand, 35 B; 0x81: the bytes after the ID */
};
struct sw_ctx {
    u8 usb;                     /* on USB: 64-byte reports, the 0x80 handshake */
    u8 mac[6];                  /* the pad's Bluetooth address, MSB first */
    volatile u8 mode;           /* input report mode: SW_ID_STATE / SW_ID_SIMPLE, 0 none yet */
    volatile u8 imu_on;         /* subcommand 0x40 */
    volatile u8 head, tail;
    struct sw_reply ring[SW_REPLIES];
    struct sw_reply last;       /* the last reply taken, for sw_reply_unsent */
    u8 resend;
    u8 hold;                    /* set by the caller: no reply now (its reply slot is full) */
    u32 state_ms;               /* when the last state report went */
    u8 simple_last[SW_SIMPLE_LEN];
    s16 imu[3][6];              /* the last 3 ticks' IMU, newest first, the Switch's frame */
    u8 imu_n;
};

/* Fresh state (a host connected). usb: the transport. */
void sw_reset(struct sw_ctx *x, int usb, const u8 *mac);

/* The host's output report, ID first: subcommands queue their reply, the
 * mode and IMU flags change, and *e says what the pad should do. 0: not
 * a report this mode knows. */
int sw_host_output(struct sw_ctx *x, const u8 *c, int len, struct sw_effect *e);

/* Every tick (the IMU history needs each one): the report to send now, if
 * any. A queued reply first, else the state when its period is up (USB:
 * once the host's 80 04 started it; none while x->hold). Returns the length after the ID (0:
 * nothing now), the ID in *id, the bytes in rep (SW_REPORT_MAX, zeroed
 * past the length). */
int sw_next_report(struct sw_ctx *x, const struct gp_state *s, const struct gp_battery *b,
                   u32 now_ms, u8 *id, u8 *rep);

/* The transport couldn't send the reply sw_next_report gave: again next. */
void sw_reply_unsent(struct sw_ctx *x);

/* State report periods, as a real Pro Controller: 15 ms on Bluetooth
 * (the console's rate; it is sensitive to more), 8 ms on USB. */
#define SW_BT_PERIOD_MS     15
#define SW_USB_PERIOD_MS    8

/* HD rumble: one actuator's 4 bytes -> its two bands' amplitudes, 0..255. */
void sw_rumble_bands(const u8 *r, u8 *high, u8 *low);
/* Both actuators' 8 bytes (left, right) -> the pad's two motors. The big
 * motor plays the left actuator's low band, the small one the right
 * actuator's high band: SDL sends its low / high channels as the bands, the
 * same on both actuators. With one actuator silent, the other one's motor
 * takes the stronger of its bands: hid-nintendo's strong / weak (left /
 * right, both bands alike), a game rumbling one side. */
void sw_rumble_motors(const u8 *r8, u8 *big, u8 *small);

/* gp_switch.c: the pad's contexts and the effects applied. */
extern struct sw_ctx sw_bt_ctx, sw_usb_ctx;
void sw_output(struct sw_ctx *x, const u8 *c, int len);   /* any context: parse, apply */

#endif
