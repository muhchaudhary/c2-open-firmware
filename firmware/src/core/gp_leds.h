/*
 * gp_leds.h: what the LED chain shows (gp_board_info.leds for the layout).
 * Connected: the host's colours and the player number. No host: the Home
 * LED breathes (reconnecting / waiting) or flashes (pairing mode). Low
 * battery: Home flashes red briefly every 2 s, either way.
 */
#ifndef GP_LEDS_H
#define GP_LEDS_H

#include "typedef.h"
#include "gamepad.h"

extern u8 gp_led_rgb[GP_LEDS_MAX][3];   /* the frame; gp_leds_send() shows it */

void gp_leds_send(void);
void gp_leds_dark(void);                /* all off (the supply stays as it is) */
void gp_leds_update(void);              /* host colours + player LEDs */
void gp_leds_set_mode_colour(const u8 *rgb);   /* the input mode's colour (16 = full): Home's default */
enum { GP_LEDS_LINK_BT, GP_LEDS_LINK_USB, GP_LEDS_LINK_RECEIVER };
void gp_leds_set_link(int link);        /* lit player LEDs: Bluetooth blue, USB white, receiver purple */
void gp_leds_set_searching(int on);     /* receiver link, not connected: all four player LEDs purple */
/* Poll, before gp_leds_home_anim: low battery, Home flashes red over the rest. */
void gp_leds_low_battery(int low, int connected);
void gp_leds_set_lowbat_rgb(const u8 *rgb);     /* the warning's colour on Home (default red) */

/* From the host (sinput_command), any context. */
void gp_leds_set_player(u8 player);     /* 0 = unassigned, 1..4 */
void gp_leds_set_host_rgb(const u8 *rgb);
void gp_leds_set_per_led(const u8 *rgb8, u8 incl_players);

/* The active profile on the player LEDs for a moment (it changed). */
void gp_leds_show_profile(void);
void gp_leds_poll(void);                /* every GP_POLL_MS */

/* Poll, while no host is connected. */
void gp_leds_home_anim(int pairing);

#endif
