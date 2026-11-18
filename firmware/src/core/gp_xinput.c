/* gp_xinput.c: see gp_xinput.h. */
#include "system/includes.h"
#include "gp_xinput.h"
#include "gp_leds.h"
#include "gp_rumble.h"
#include "gp_log.h"

/* The state encoder and the USB descriptors: gp_xinput_desc.c (shared with the receiver). */

/* LED patterns (as xpad / the 360 sets them): 0 off, 1 all blinking, 2..5
 * player 1..4 flashes then on, 6..9 player 1..4 on, 10.. animations. The
 * pad shows the player number either way. */
static void xinput_led(u8 n)
{
    u8 player = 0;
    if (n >= 2 && n <= 5) {
        player = n - 1;
    } else if (n >= 6 && n <= 9) {
        player = n - 5;
    }
    gp_leds_set_player(player);
}

void xinput_command(const u8 *c, int len)
{
    if (len >= 5 && c[0] == 0x00 && c[1] == 0x08) {      /* rumble: big (left), small (right) */
        gp_rumble_set(c[3], c[4]);
    } else if (len >= 3 && c[0] == 0x01 && c[1] == 0x03) {
        xinput_led(c[2]);
    }
}
