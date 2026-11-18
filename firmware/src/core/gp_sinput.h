/*
 * gp_sinput.h: the SInput protocol (docs/sinput.md 2), transport-free:
 * the HID report descriptor, input 0x01 (state) and 0x02 (features reply)
 * encoding, and the output 0x03 commands from the host.
 */
#ifndef GP_SINPUT_H
#define GP_SINPUT_H

#include "typedef.h"
#include "gamepad.h"

#define SINPUT_REPORT_LEN   63          /* input 0x01 / 0x02, after the report ID */
#define SINPUT_ID_STATE     0x01
#define SINPUT_ID_FEATURES  0x02
#define SINPUT_ID_COMMAND   0x03
#define SINPUT_ID_BATTERY   0x04        /* ours, for Linux: battery % + charging, sent on a change (63 B) */

#define SINPUT_REPORT_DESC_LEN  158
extern const u8 sinput_report_desc[SINPUT_REPORT_DESC_LEN];

void sinput_encode_state(u8 *rep, const struct gp_state *s, const struct gp_battery *b, u32 ts_us);
void sinput_encode_features(u8 *rep, const struct gp_sinput_caps *caps, const u8 *serial6);

/* Input 0x04 (Linux's power_supply): due at once after sinput_battery_reset()
 * (a new host), when the percent or the 5 V state changed, and every
 * SINPUT_BATTERY_MS. A due call counts as sent: encode and post it then. */
#define SINPUT_BATTERY_MS   60000
void sinput_battery_reset(void);
int  sinput_battery_due(const struct gp_battery *b, u32 now_ms);
void sinput_encode_battery(u8 *rep, const struct gp_battery *b);

/* Output report 0x03: c = the bytes after the report ID. Any context. */
void sinput_command(const u8 *c, int size);

/* Features reply: the host asked (pending), and the reply in flight answers
 * that request (asked: for the debug log). */
extern volatile u8 sinput_feat_pending;
extern volatile u8 sinput_feat_asked;
extern volatile u8 sinput_feat_retry;     /* an unasked features reply found the pool full: send one again */

#endif
