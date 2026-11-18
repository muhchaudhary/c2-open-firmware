/* gp_sinput.c: see gp_sinput.h. Follows the earlier SInput hybrid, a patch
 * of the stock firmware (docs/sinput.md 2). */
#include "system/includes.h"
#include "gp_sinput.h"
#include "gp_config.h"
#include "gp_leds.h"
#include "gp_rumble.h"
#include "gp_log.h"

/* The report descriptor: gp_sinput_desc.c (shared with the receiver). */

volatile u8 sinput_feat_pending;
volatile u8 sinput_feat_asked;
volatile u8 sinput_feat_retry;

/* The state and features encoders: gp_sinput_desc.c (shared with the receiver). */

void sinput_command(const u8 *c, int size)
{
    switch (c[0]) {
    case 0x02:                                           /* features request */
        gp_log("host: features request");
        sinput_feat_pending = 1;
        break;
    case 0x03:                                           /* player number 1..4 (0: unassigned) */
        if (size >= 2 && c[1] <= 4) {
            gp_leds_set_player(c[1]);
        }
        break;
    case 0x04:                                           /* one colour on Home + grips */
        if (size >= 4) {
            gp_leds_set_host_rgb(c + 1);
        }
        break;
    case 0x80:                                           /* vendor: 8 slots R G B, then a flag (docs/sinput.md 2.5) */
        if (size >= 26) {
            gp_leds_set_per_led(c + 1, c[25]);
        }
        break;
    case GP_CFG_CMD:                                     /* ours: the configuration protocol (gp_config.h) */
        gp_config_request(c, size);
        break;
    case 0x01:                                           /* haptics: type 2 = La Lb Ra Rb */
        if (size >= 6 && c[1] == 0x02) {
            gp_rumble_set(c[2], c[4]);
        }
        break;
    default:
        break;
    }
}
