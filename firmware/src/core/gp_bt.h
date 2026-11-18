/*
 * gp_bt.h: the Bluetooth Classic HID side: the SDK's BT stack set up as a
 * gamepad, the report transport (gp_bt_transport), pairing mode, and the
 * safety nets that keep a link usable.
 */
#ifndef GP_BT_H
#define GP_BT_H

#include "typedef.h"
#include "system/event.h"
#include "gp_transport.h"

void gp_bt_init(void);                  /* app start: BT stack up (async: BT_STATUS_INIT_OK follows) */
void gp_bt_after_init(void);            /* BT_STATUS_INIT_OK: reconnect to the last host, or wait */
int  gp_bt_start(void);                 /* before any host connects: 0 = reports can't go out */
int  gp_bt_event(struct sys_event *e);  /* app task events: 1 if handled */
void gp_bt_poll(u8 connected);          /* every GP_POLL_MS */
int  gp_bt_pairing(void);               /* pairing mode (until a host connects) */
void gp_bt_pairing_start(void);         /* poll context */
void gp_bt_suspend(int on);             /* wired (USB): no link, not connectable; off: reconnect */

/* Power-off: close the link, keep the host out. */
int  gp_bt_link_up(void);
int  gp_bt_receiver(void);                   /* the receiver link (gp_settings.link) */
void gp_bt_disconnect(void);
void gp_bt_scan_off(void);
void gp_bt_shutdown(void);              /* the BT stack down */

/* The longest report after its ID: DS4 mode's 0x11 (77 B; SInput's 63). */
#define GP_BT_REPORT_MAX    77
/* patch_sdk.py: GET_REPORT on the control channel `cid` (BT task). 1 when
 * it sent the reply itself, 0 to leave it to the library. */
int  gp_bt_get_report(u16 cid, const u8 *packet, int size);

/* Debug log. */
extern u32 gp_bt_sent;                  /* reports handed to the HID stack */
extern volatile int gp_hid_send_err;    /* patch_sdk.py: the last failed HID send's code */
extern volatile u32 gp_hid_send_errs;   /* ... and how many failed */

#endif
