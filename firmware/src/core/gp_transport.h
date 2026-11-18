/*
 * gp_transport.h: how reports reach the host. The report tick asks the
 * active transport whether a host is there and free, and posts at most one
 * report per tick; each transport delivers it in its own way and context
 * and hands the host's output reports to the protocol (sinput_command,
 * xinput_command, ds4_output, sw_output).
 */
#ifndef GP_TRANSPORT_H
#define GP_TRANSPORT_H

#include "typedef.h"

enum gp_protocol {
    GP_PROTO_SINPUT,                            /* gp_sinput.c */
    GP_PROTO_XINPUT,                            /* gp_xinput.c: Xbox 360, USB */
    GP_PROTO_XBOXBT,                            /* gp_xboxbt.c: Xbox One S, Bluetooth */
    GP_PROTO_LINK,                              /* gp_link.h: our own, to the C2 receiver */
    GP_PROTO_DS4,                               /* gp_ds4.h: DualShock 4, USB (report 0x01) */
    GP_PROTO_DS4BT,                             /* gp_ds4.h: DualShock 4, Bluetooth (report 0x11) */
    GP_PROTO_SWITCH,                            /* gp_switch.h: Switch Pro Controller, USB */
    GP_PROTO_SWITCHBT,                          /* gp_switch.h: Switch Pro Controller, Bluetooth */
};

struct gp_transport {
    const char *name;
    int  (*connected)(void);                    /* a host has the gamepad open */
    int  (*busy)(void);                         /* the last report is still going out */
    int  (*protocol)(void);                     /* enum gp_protocol: what to post */
    /* tick: one report. SInput, Xbox, DS4: the ID and the bytes after it;
     * XInput: id 0 and the whole message. */
    void (*post)(u8 id, const u8 *report, int len);
};

extern const struct gp_transport gp_bt_transport;     /* gp_bt.c: Bluetooth Classic HID */

#endif
