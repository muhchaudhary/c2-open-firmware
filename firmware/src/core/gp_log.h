/*
 * gp_log.h: a debug log over USB serial (CDC ACM, /dev/ttyACM*), next to the
 * Bluetooth link. gp_log() works from any context (tasks, timer interrupts):
 * it formats into a 4 KB ring (oldest lines dropped when full) and the app
 * log task sends the ring out while a terminal has the port open, so the lines
 * from before opening it come out first. Read it with tools/c2log.py.
 *
 * Commands typed into the port (one per line): "boot" enters the USB
 * bootloader (as Home + Options), "reset" restarts, "help".
 */
#ifndef GP_LOG_H
#define GP_LOG_H

#include "typedef.h"
#include "system/event.h"

void gp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gp_log_init(void);                          /* app task: starts the flush timer */
void gp_log_usb_attach(int usb_id);              /* gp_usb.c: CDC up on usb_id, -1 = gone */

/* Configuration over the log port: "cfg <hex>" from the host calls
 * gp_log_cfg_request (log task; the pad: gp_config.c, the receiver: to the
 * pad); gp_log_cfg_reply prints a reply as "cfg> <hex>" (any context). */
void gp_log_cfg_request(const u8 *req, int len);
void gp_log_cfg_reply(const u8 *rep, int len);

/* Bounded hardware waits that ran out (each would have hung the chip). */
extern volatile u8 gp_hang_count;

#endif
