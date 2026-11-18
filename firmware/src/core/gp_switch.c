/* gp_switch.c: the pad's Switch-mode contexts and the host's effects
 * applied (gp_switch.h; the protocol is in gp_switch_desc.c). Rumble: the
 * low band to the big motor, the high band to the small one
 * (sw_rumble_motors). Player lights: the player LEDs. Home's light (subcommand
 * 0x38) is left alone: Home shows the mode's red. */
#include "system/includes.h"
#include "gp_switch.h"
#include "gp_leds.h"
#include "gp_rumble.h"

struct sw_ctx sw_bt_ctx, sw_usb_ctx;

void sw_output(struct sw_ctx *x, const u8 *c, int len)
{
    struct sw_effect e;
    if (!sw_host_output(x, c, len, &e)) {
        return;
    }
    if (e.valid & SW_FX_RUMBLE) {
        gp_rumble_set(e.big, e.small);
    }
    if (e.valid & SW_FX_PLAYER) {
        gp_leds_set_player(e.player);
    }
}
