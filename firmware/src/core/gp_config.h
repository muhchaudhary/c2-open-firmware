/*
 * gp_config.h: the configuration protocol (docs/config-protocol.md).
 *
 * Requests come in SInput's output report 0x03 with command byte
 * GP_CFG_CMD (next to the per-LED vendor command 0x80); replies go out in
 * input report 0x02 with the same first byte, which SDL ignores outside
 * its init (it looks for 02 02 there). So it works over Bluetooth and USB,
 * in Steam mode, while SDL or Steam has the pad open.
 *
 * Request (after the report ID, 47 B): cmd 0xC0, seq, op, payload.
 * Reply   (after the report ID, 63 B): cmd 0xC0, seq, op, status, payload.
 *
 * Contexts: gp_config_request runs where the output report arrives (the
 * BT stack's task, the USB interrupt); it only copies the request. The
 * poll answers it (settings, restarts), the tick sends the reply.
 */
#ifndef GP_CONFIG_H
#define GP_CONFIG_H

#include "typedef.h"

#define GP_CFG_CMD          0xC0
#define GP_CFG_PROTO        3           /* protocol version, in the INFO reply (2: calibration; 3: status 6,
                                         * the release number, the log port) */

/* Operations */
enum {
    GP_CFG_OP_INFO = 0x00,              /* -> proto, fw, board, link, battery, setting count */
    GP_CFG_OP_LIST = 0x01,              /* index -> key, flags, min, max, default, value, name */
    GP_CFG_OP_GET = 0x02,               /* key -> key, value */
    GP_CFG_OP_SET = 0x03,               /* key, value -> key, value as applied */
    GP_CFG_OP_DEFAULTS = 0x04,          /* -> all settings back to their defaults */
    GP_CFG_OP_REBOOT = 0x05,            /* -> restart after the reply */
    GP_CFG_OP_BOOTLOADER = 0x06,        /* -> the USB bootloader after the reply (USB 5 V needed) */
    /* Calibration (gp_cal.h) */
    GP_CFG_OP_CAL_START = 0x10,         /* mask (GP_CAL_*) -> mask: the rest step begins */
    GP_CFG_OP_CAL_STATUS = 0x11,        /* -> phase, mask, stored, progress, restarts, short, live values */
    GP_CFG_OP_CAL_FINISH = 0x12,        /* -> short mask (status 3 if any), stored mask */
    GP_CFG_OP_CAL_CANCEL = 0x13,
    GP_CFG_OP_CAL_CLEAR = 0x14,         /* mask -> stored mask: those parts learn at runtime again */
    GP_CFG_OP_CAL_GET = 0x15,           /* -> the stored calibration */
};

/* Reply status */
enum {
    GP_CFG_OK = 0,
    GP_CFG_ERR_OP = 1,                  /* unknown operation */
    GP_CFG_ERR_KEY = 2,                 /* unknown setting / index */
    GP_CFG_ERR_VALUE = 3,               /* value out of range */
    GP_CFG_ERR_STATE = 4,               /* not now (bootloader without USB) */
    GP_CFG_ERR_LEN = 5,                 /* request too short */
    GP_CFG_ERR_STORE = 6,               /* done, but not saved: in use until power-off */
};

/* Setting keys and flags: gp_settings.c's table (LIST describes them). */
#define GP_CFG_KEY_PROFILE  4

void gp_config_request(const u8 *c, int size);  /* any context: the request after the report ID */
void gp_config_poll(void);                      /* every GP_POLL_MS: answers a waiting request */
int  gp_config_reply(u8 *rep);                  /* tick: the reply to send (SINPUT_REPORT_LEN), 1 if there is one */
void gp_config_retry(void);                     /* the reply didn't go out: send it again */

#endif
