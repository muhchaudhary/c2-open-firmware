/* dongle.h: the C2 receiver's firmware (docs/dongle.md). */
#ifndef DONGLE_H
#define DONGLE_H

#include "typedef.h"
#include "system/event.h"

#define DG_STABLE_MS        10000           /* uptime that clears the crash-loop guard */
#define DG_BUTTON_POLL_MS   20

/* board_dongle.c */
extern u8 dg_boot_count, dg_boot_rst_src;
void dg_enter_bootloader(void);             /* the JieLi USB bootloader; doesn't return */
void dg_guard_stable(void);
void dg_crumb(int n);                       /* breadcrumb: see board_dongle.c */
void dg_led(int on);
void dg_checkpoint(int n);                  /* breadcrumb (dg_crumb) */
void dg_button_init(void);
int dg_button_pressed(void);                /* PA00 low */

u32 dg_us(void);                            /* microseconds since start-up (dg_main.c) */

/* Latency (dg_usb.c / dg_bt.c; the log's `stats` command shows it):
 *  - wait: a state report from Bluetooth until it is on the USB endpoint;
 *  - echo: the host's ping (03 8E, dg_link.c), timed at the receiver: USB
 *    -> the hello request sent on BT, sent -> the pad's hello back, and the
 *    answer (02 8E) until it is on the USB endpoint (tools/c2lat.py). */
struct dg_lat {
    u32 wait_n, wait_sum, wait_max;
    u32 echo_n, out_sum, out_max, air_sum, air_max, usb_sum, usb_max;
};
extern volatile struct dg_lat dg_lat;
extern volatile u8 dg_echo_state;           /* 0 idle, 1 asked (t0), 2 sent on BT (t1), 3 answer queued (t2) */
extern volatile u32 dg_echo_t0, dg_echo_t1, dg_echo_t2;

/* dg_bt.c */
void dg_bt_init(void);
int dg_bt_event(struct sys_event *e);
int dg_bt_command(const char *cmd);
void dg_bt_output(u8 id, const u8 *data, u16 len);   /* host -> pad, any context */
void dg_bt_pair_start(void);                        /* app task */
u8 dg_bt_state(void);                               /* 0 idle, 1 connected, 2 pairing */

/* dg_usb.c */
int dg_usb_event(struct sys_event *e);
void dg_usb_forward(const u8 *pkt, u16 len);          /* pad -> host: an `a1 <id> ...` packet */
void dg_usb_put(u8 id, const u8 *data, u16 len);      /* a report to the host: id, then the bytes after it */
void dg_usb_queue(u8 id, const u8 *data, u16 len);    /* the same from the USB interrupt: sent by the 2 ms tick */
#define DG_ID_XINPUT        0               /* dg_usb_put id: a whole 20-byte XInput message (Xbox mode) */
void dg_usb_load_mode(void);                /* start-up: the saved mode */
void dg_usb_set_mode(int mode);             /* enum gp_mode: a pad spoke (hello / SInput report): the gamepad in its mode */
void dg_usb_pad_gone(void);                 /* the pad disconnected: no gamepad on USB (re-enumerates) */
int  dg_usb_xinput(void);                   /* up as XInput (Xbox mode) */
int  dg_usb_ds4(void);                      /* up as the DualShock 4 (DS4 mode) */
int  dg_usb_switch(void);                   /* up as the Pro Controller (Switch mode) */
struct gp_state;
struct gp_battery;
void dg_usb_switch_state(const struct gp_state *s, const struct gp_battery *b, u32 now_ms);   /* BT task */
int  dg_usb_sinput_up(void);                /* up as the SInput gamepad (Steam mode) */
void dg_usb_poll(void);                     /* app task, every 100 ms */

/* dg_link.c: the pad's receiver-link messages (core/gp_link.h) */
#define DG_PING_CMD         0x8E            /* host `03 8E` -> pad hello -> host `02 8E` (tools/c2lat.py) */
void dg_link_in(u8 id, const u8 *p, int len);
void dg_link_reset(void);
int  dg_link_active(void);                  /* a hello is in: the pad speaks the link */
int  dg_link_features(void);                /* answer SInput's features request here (0: no hello) */
void dg_link_ping(void);
const u8 *dg_link_serial(void);            /* the pad's Bluetooth address, MSB first (zeros: no hello) */
extern volatile u32 dg_link_states, dg_link_lost;
extern volatile u32 dg_usb_sent, dg_usb_dropped, dg_usb_flushed, dg_usb_resetups;
void dg_usb_dump(void);                      /* the `usb` log command */

#endif
