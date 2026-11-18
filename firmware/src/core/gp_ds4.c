/* gp_ds4.c: the host's DS4 output report applied (gp_ds4.h; the parsing
 * is in gp_ds4_desc.c). The lightbar colour goes to Home and the grips, as
 * SInput's 03 04: hid-playstation sets it from the player number (1 blue,
 * 2 red, 3 green, 4 pink), SDL and Steam from their own colours. */
#include "system/includes.h"
#include "gp_ds4.h"
#include "gp_leds.h"
#include "gp_rumble.h"

void ds4_output(const u8 *c, int len)
{
    struct ds4_out o;
    if (!ds4_parse_output(c, len, &o)) {
        return;
    }
    if (o.valid & DS4_OUT_RUMBLE) {
        gp_rumble_set(o.big, o.small);
    }
    if (o.valid & DS4_OUT_LIGHTBAR) {
        gp_leds_set_host_rgb(o.rgb);
    }
}
