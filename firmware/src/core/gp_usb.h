/*
 * gp_usb.h: the USB device side. With a host on the cable the pad is a
 * USB serial debug log. By default that is all (under the log's own IDs)
 * and the pad stays on Bluetooth. USB input (gp_usb_set_input: Home +
 * Capture tapped) adds the gamepad in the input mode's USB protocol (Steam:
 * SInput, the same reports as over Bluetooth; Xbox: XInput, a wired Xbox
 * 360 pad): once the host has opened it, USB is the active transport and
 * Bluetooth steps aside; input off or unplugged, Bluetooth reconnects. USB
 * input starts off with every cable. Charging mode: the log alone.
 */
#ifndef GP_USB_H
#define GP_USB_H

#include "typedef.h"
#include "system/event.h"
#include "gp_transport.h"

extern const struct gp_transport gp_usb_transport;

int  gp_usb_event(struct sys_event *e);         /* OTG cable events: 1 if handled */
int  gp_usb_host_ready(void);                   /* the host has the gamepad open */
int  gp_usb_host_present(void);                 /* a USB host on the cable */
void gp_usb_set_input(int on);                  /* poll: the gamepad on USB (re-enumerates) */
int  gp_usb_input_on(void);
void gp_usb_log_only(void);                     /* charging mode: re-enumerate as the debug log only */
void gp_usb_poll(void);                         /* every GP_POLL_MS: XInput readiness */

#endif
