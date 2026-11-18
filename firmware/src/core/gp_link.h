/*
 * gp_link.h: the pad <-> C2 receiver link protocol (docs/dongle.md, "The
 * receiver link"), shared by the pad (gp_core.c, gp_bt.c) and the receiver
 * (boards/dongle). Only on the receiver link: the pad's own Bluetooth
 * identity for the receiver, so the format is ours.
 *
 * On the HID interrupt channel, as HIDP DATA packets: `a1 <id> <payload>`
 * pad -> receiver, `a2 <id> <payload>` receiver -> pad. The receiver
 * translates to what it shows on USB (SInput now; XInput next).
 *
 * Pad -> receiver
 *   GP_LINK_ID_STATE   every tick: sequence (of reports sent), buttons,
 *                      sticks, triggers, IMU, the pad's ms clock. 30 B, so with the HIDP and
 *                      L2CAP headers (36 B) it fits one 2-DH1 packet (max
 *                      54 B), where SInput's 63-byte report needed 3-DH1.
 *   GP_LINK_ID_STATUS  battery and charging: on a change and every
 *                      GP_LINK_STATUS_MS, not in every state report.
 *   GP_LINK_ID_HELLO   at the connect and when asked: protocol version,
 *                      the mode, SInput capabilities, serial. The receiver
 *                      answers SInput's features request from it.
 *   SInput 0x02        configuration replies (gp_config.h), unchanged.
 * Receiver -> pad
 *   GP_LINK_ID_HELLO_REQ  send a hello (the receiver hasn't one).
 *   SInput 0x03        host commands (rumble, LEDs, configuration),
 *                      unchanged: the pad's sinput_command takes them.
 */
#ifndef GP_LINK_H
#define GP_LINK_H

#include "typedef.h"
#include "gamepad.h"

#define GP_LINK_VERSION         1

#define GP_LINK_ID_STATE        0x20
#define GP_LINK_ID_STATUS       0x21
#define GP_LINK_ID_HELLO        0x22
#define GP_LINK_ID_HELLO_REQ    0x30

#define GP_LINK_STATE_LEN       30
#define GP_LINK_STATUS_LEN      5
#define GP_LINK_HELLO_LEN       22
#define GP_LINK_STATUS_MS       1000

struct gp_link_hello {
    u8  version;                /* GP_LINK_VERSION */
    u8  mode;                   /* enum gp_mode: what the receiver shows on USB */
    struct gp_sinput_caps caps;
    u8  serial[6];              /* as the SInput features reply has it */
    u16 tick_us;                /* the pad's report interval */
};

/* State: seq (wraps), the state, the pad's millisecond clock (wraps). */
void gp_link_encode_state(u8 *p, u8 seq, const struct gp_state *s, u16 ms);
int  gp_link_decode_state(const u8 *p, int len, u8 *seq, struct gp_state *s, u16 *ms);

void gp_link_encode_status(u8 *p, const struct gp_battery *b);
int  gp_link_decode_status(const u8 *p, int len, struct gp_battery *b);

void gp_link_encode_hello(u8 *p, const struct gp_link_hello *h);
int  gp_link_decode_hello(const u8 *p, int len, struct gp_link_hello *h);

#endif
