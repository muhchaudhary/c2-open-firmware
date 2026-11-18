/* gp_rumble.c: see gp_rumble.h. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_settings.h"
#include "gp_rumble.h"
#include "erm_rumble.h"

#define GP_RUMBLE_FEEDBACK_MS    1000       /* stock: 1 s at the new level after M + Up / Down */

static volatile u8 gp_want[2];              /* from the host */
static u8 gp_feedback;                      /* level feedback running, since gp_feedback_t0 */
static u32 gp_feedback_t0;
static volatile u32 gp_want_ms;             /* host rumble duration, 0 = no limit ... */
static volatile u32 gp_want_t0;             /* ... from this time (gp_ms) */

void gp_rumble_init(void)
{
    erm_rumble_init(gp_board_info.motors, 2);
}

void gp_rumble_set_timed(u8 left, u8 right, u32 ms)
{
    /* The tick may interrupt this: the duration goes last, so it never
     * pairs with an old start time. */
    gp_want_ms = 0;
    gp_want_t0 = gp_ms();
    gp_want[0] = left;
    gp_want[1] = right;
    gp_want_ms = ms;
}

void gp_rumble_set(u8 left, u8 right)
{
    gp_rumble_set_timed(left, right, 0);
}

static void gp_rumble_apply(void)
{
    static u32 last;
    u32 now = gp_ms();
    u32 dt = now - last;                                 /* for the motors' kick */
    last = now;
    if (dt > 100) {
        dt = 100;                                        /* the first call, or after a pause */
    }
    if (gp_feedback && now - gp_feedback_t0 >= GP_RUMBLE_FEEDBACK_MS) {
        gp_feedback = 0;
    }
    u32 want_ms = gp_want_ms;
    if (want_ms && now - gp_want_t0 >= want_ms) {
        gp_want_ms = 0;
        gp_want[0] = gp_want[1] = 0;                     /* the host's duration is over */
    }
    for (int i = 0; i < 2; i++) {
        erm_rumble_update(i, gp_feedback ? 255 : gp_want[i], gp_settings.rumble_level, dt);
    }
}

static void gp_rumble_level_keys(u32 buttons)
{
    static u8 last;
    u8 now = 0;
    if (buttons & GP_BIT(GP_BTN_MODIFIER)) {
        now = ((buttons & GP_BIT(GP_BTN_UP)) ? 1 : 0) | ((buttons & GP_BIT(GP_BTN_DOWN)) ? 2 : 0);
    }
    u8 pressed = now & ~last;
    last = now;
    u8 level = gp_settings.rumble_level;
    if (pressed & 1 && level < ERM_LEVELS - 1) {
        level++;
    } else if (pressed & 2 && level > 0) {
        level--;
    } else if (!pressed) {
        return;
    }
    gp_settings.rumble_level = level;
    erm_rumble_rekick();
    gp_settings_changed();                               /* saved 2 s after the last change */
    gp_feedback = level != 0;
    gp_feedback_t0 = gp_ms();
}

void gp_rumble_tick(u32 buttons)
{
    gp_rumble_level_keys(buttons);
    gp_rumble_apply();
}

void gp_rumble_stop(void)
{
    gp_want[0] = gp_want[1] = 0;
    gp_feedback = 0;
    gp_rumble_apply();
}
