/*
 * gp_xboxbt.h: the Xbox One S controller over Bluetooth (045E:02FD, Bluetooth
 * Classic HID), as stock's Bluetooth Xbox mode presents itself: Windows
 * takes it with its built-in driver (XInput), Linux / SDL as an Xbox pad.
 * Transport-free: the report descriptor, the input reports and the host's
 * rumble command.
 */
#ifndef GP_XBOXBT_H
#define GP_XBOXBT_H

#include "typedef.h"
#include "gamepad.h"

#define XBOXBT_REPORT_DESC_LEN  334
#define XBOXBT_REPORT_LEN       16      /* report 1, after the ID */
#define XBOXBT_ID_STATE         0x01
#define XBOXBT_ID_GUIDE         0x02    /* 1 byte: bit 0 the Xbox button */
#define XBOXBT_ID_RUMBLE        0x03    /* output */
#define XBOXBT_ID_BATTERY       0x04    /* 1 byte: 0..255 */

extern const u8 xboxbt_report_desc[XBOXBT_REPORT_DESC_LEN];
extern volatile u8 xboxbt_guide_dirty;          /* report 2 to (re)send: the transport clears it once sent */

void xboxbt_encode_state(u8 *rep, const struct gp_state *s);
void xboxbt_command(const u8 *c, int len);      /* output report 3, after the ID; any context */

#endif
