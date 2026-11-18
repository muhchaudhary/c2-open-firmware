/* gp_leds.c: see gp_leds.h. */
#include "system/includes.h"
#include "gp_leds.h"
#include "gp_settings.h"

u8 gp_led_rgb[GP_LEDS_MAX][3];

static u8 gp_player;                    /* 0 = unassigned, 1..4 */
static u8 gp_host_rgb[3] = {16, 16, 16};
static u8 gp_per_led[GP_LEDS_MAX][3];
static u8 gp_per_led_on;                /* 0 none, 1 all but the player LEDs, 2 all */
static u8 gp_link_rgb[3] = {0, 0, 16};  /* lit player LEDs: the link (Bluetooth blue, USB white, receiver purple) */
#define PROFILE_SHOW_MS     1000
static volatile u16 gp_prof_ms;         /* showing the profile number (purple, the profile's LED) */
static u8 gp_searching;                 /* receiver link, not connected: all player LEDs purple */
static const u8 gp_receiver_rgb[3] = {10, 0, 16};

/* Low battery: Home flashes red, LOWBAT_ON_MS every LOWBAT_PERIOD_MS, over
 * whatever it shows otherwise (the host's colour, or the breathing). */
#define LOWBAT_PERIOD_MS    2000
#define LOWBAT_ON_MS        300
static u8 gp_lowbat_rgb[3] = {40, 0, 0};
static u8 gp_lowbat, gp_lowbat_lit;
static u16 gp_lowbat_t;

void gp_leds_send(void)
{
    gp_board_led_send((const u8 (*)[3])gp_led_rgb, gp_board_info.leds.count);
}

void gp_leds_dark(void)
{
    memset(gp_led_rgb, 0, sizeof(gp_led_rgb));
    gp_leds_send();
}

/* Player LED k (0..3) lit for player numbers above k; -1 if the slot isn't one. */
static int player_index(int slot)
{
    for (int k = 0; k < 4; k++) {
        if (gp_board_info.leds.player[k] == slot) {
            return k;
        }
    }
    return -1;
}

/* The player LEDs show the player number in the link's colour; with no
 * number from the host (Windows over Bluetooth never sends one) player 1,
 * so the link always shows. */
void gp_leds_update(void)
{
    static const u8 off[3] = {0, 0, 0}, prof_rgb[3] = {12, 0, 16};
    u8 player = gp_player ? gp_player : 1;
    for (int i = 0; i < gp_board_info.leds.count; i++) {
        int k = player_index(i);
        const u8 *c = gp_host_rgb;
        if (k >= 0) {
            c = (k < player) ? gp_link_rgb : off;
        }
        if (gp_per_led_on == 2 || (gp_per_led_on == 1 && k < 0)) {
            c = gp_per_led[i];
        }
        if (k >= 0 && gp_searching) {
            c = gp_receiver_rgb;                         /* receiver link, not connected */
        }
        if (k >= 0 && gp_prof_ms) {
            c = k == gp_settings.profile ? prof_rgb : off;   /* the profile number, for a moment */
        }
        if (i == gp_board_info.leds.home && gp_lowbat_lit) {
            c = gp_lowbat_rgb;
        }
        memcpy(gp_led_rgb[i], c, 3);
    }
    gp_leds_send();
}

void gp_leds_low_battery(int low, int connected)
{
    u8 lit = 0;
    if (low) {
        gp_lowbat_t = gp_lowbat_t + GP_POLL_MS < LOWBAT_PERIOD_MS ? gp_lowbat_t + GP_POLL_MS : 0;
        lit = gp_lowbat_t < LOWBAT_ON_MS;
    } else {
        gp_lowbat_t = 0;
    }
    gp_lowbat = low;
    if (lit != gp_lowbat_lit) {
        gp_lowbat_lit = lit;
        if (connected) {
            gp_leds_update();                            /* not connected: gp_leds_home_anim draws it */
        }
    }
}

void gp_leds_set_lowbat_rgb(const u8 *rgb)
{
    memcpy(gp_lowbat_rgb, rgb, 3);
}

void gp_leds_set_searching(int on)
{
    if (on != gp_searching) {
        gp_searching = on;
        gp_leds_update();
    }
}

void gp_leds_show_profile(void)
{
    gp_prof_ms = PROFILE_SHOW_MS;
    gp_leds_update();
}

void gp_leds_poll(void)
{
    if (gp_prof_ms && (gp_prof_ms = gp_prof_ms > GP_POLL_MS ? gp_prof_ms - GP_POLL_MS : 0) == 0) {
        gp_leds_update();                                /* back to the player number */
    }
}

void gp_leds_set_link(int link)
{
    static const u8 rgb[3][3] = {
        {0, 0, 16},                                      /* GP_LEDS_LINK_BT: blue */
        {16, 16, 16},                                    /* GP_LEDS_LINK_USB: white */
        {10, 0, 16},                                     /* GP_LEDS_LINK_RECEIVER: purple (gp_receiver_rgb) */
    };
    memcpy(gp_link_rgb, rgb[link < 3 ? link : 0], 3);
    gp_leds_update();
}

void gp_leds_set_player(u8 player)
{
    gp_player = player;
    gp_leds_update();
}

void gp_leds_set_host_rgb(const u8 *rgb)
{
    memcpy(gp_host_rgb, rgb, 3);
    gp_per_led_on = 0;
    gp_leds_update();
}

void gp_leds_set_per_led(const u8 *rgb8, u8 incl_players)
{
    memcpy(gp_per_led, rgb8, sizeof(gp_per_led));
    gp_per_led_on = incl_players ? 2 : 1;
    gp_leds_update();
}

/* Home LED while no host is connected, as stock: it breathes slowly while
 * reconnecting / waiting and flashes in pairing mode, in the colour of the
 * input mode (gp_core.c: Steam yellow, Xbox green, DS4 blue, Switch red). Only the Home slot changes; the host's LED
 * commands only come while connected, so the two never send at once. */
#define HOME_PEAK           48              /* brightness at the top of a breath / a flash */
#define BREATH_MS           3000            /* one breath, in and out */
#define FLASH_MS            400             /* one flash period (on half, off half) */
static u8 gp_mode_rgb[3] = { 16, 16, 16 };  /* the input mode's colour (set at start: gp_core.c), 16 = full */

void gp_leds_set_mode_colour(const u8 *rgb)
{
    memcpy(gp_mode_rgb, rgb, 3);
    memcpy(gp_host_rgb, rgb, 3);                         /* also the colour until a host sets one */
}

void gp_leds_home_anim(int pairing)
{
    static u16 t;
    u32 level;
    t += GP_POLL_MS;
    if (pairing) {
        t %= FLASH_MS;
        level = t < FLASH_MS / 2 ? HOME_PEAK : 0;
    } else {
        t %= BREATH_MS;
        u32 x = t < BREATH_MS / 2 ? t : BREATH_MS - t;  /* 0 .. half, triangle */
        level = HOME_PEAK * x * x / ((BREATH_MS / 2) * (BREATH_MS / 2));   /* squared: looks even */
    }
    for (int k = 0; k < 3; k++) {
        gp_led_rgb[gp_board_info.leds.home][k] = gp_lowbat_lit ? gp_lowbat_rgb[k] : gp_mode_rgb[k] * level / 16;
    }
    gp_leds_send();
}
