/* gp_settings.c: see gp_settings.h. */
#include "system/includes.h"
#include "gamepad.h"
#include "gp_settings.h"
#include "gp_log.h"
#include "gp_store.h"

#define GP_CFG_ID           GP_STORE_SETTINGS
#define GP_CFG_MAGIC        0xC2        /* kept from the first (C2-only) layout: settings survive the update */
#define GP_CFG_VERSION      4           /* 1: rumble; 2: + mode; 3: + idle-off; 4: the struct (profiles) */
#define GP_SAVE_DELAY_MS    2000

/* ---- Keys (stable: clients store them) --------------------------------------
 * 1..: global; 0x10..: per-profile tuning; 0x40 + i: map of button i;
 * 0x60 + i: turbo of button i. */
enum {
    K_RUMBLE_LEVEL = 1, K_MODE = 2, K_IDLE_OFF_MIN = 3, K_PROFILE = 4, K_LINK = 5,
    K_STICK_DZ = 0x10, K_STICK_OUTER, K_STICK_ANTI, K_STICK_CURVE,
    K_TRIG_DZ, K_TRIG_OUTER, K_HAIR, K_TURBO_RATE,
    K_MAP = 0x40, K_TURBO = 0x60,
};

const u8 gp_map_buttons[GP_MAP_N] = {
    GP_BTN_SOUTH, GP_BTN_EAST, GP_BTN_WEST, GP_BTN_NORTH,
    GP_BTN_UP, GP_BTN_DOWN, GP_BTN_LEFT, GP_BTN_RIGHT,
    GP_BTN_L3, GP_BTN_R3, GP_BTN_L1, GP_BTN_R1,
    GP_BTN_PADDLE_L1, GP_BTN_PADDLE_R1,
    GP_BTN_START, GP_BTN_BACK, GP_BTN_GUIDE, GP_BTN_CAPTURE,
};
const char *const gp_map_names[GP_MAP_N] = {
    "a", "b", "x", "y", "up", "down", "left", "right", "l3", "r3", "lb", "rb",
    "paddle_l", "paddle_r", "start", "back", "guide", "capture",
};

/* The fixed part of the table: offsets into struct gp_settings (global) or
 * struct gp_profile (per profile). The map / turbo keys are generated. */
struct fixed {
    u8 key, flags, min, max, def, off;
    const char *name;
};
#define G(f)    offsetof(struct gp_settings, f)
#define P(f)    offsetof(struct gp_profile, f)
static const struct fixed fixed[] = {
    { K_RUMBLE_LEVEL, 0, 0, 4, 4, G(rumble_level), "rumble_level" },
    { K_MODE, GP_SET_F_RESTART, 0, GP_MODE_LAST, GP_MODE_STEAM, G(mode), "mode" },
    { K_IDLE_OFF_MIN, 0, 0, GP_IDLE_OFF_MAX, 10, G(idle_off_min), "idle_off_min" },
    { K_PROFILE, 0, 0, GP_PROFILES - 1, 0, G(profile), "profile" },
    { K_LINK, GP_SET_F_RESTART, 0, GP_LINK_RECEIVER, GP_LINK_BT, G(link), "link" },
    { K_STICK_DZ, GP_SET_F_PROFILE, 0, 50, 4, P(stick_deadzone), "stick_deadzone" },
    { K_STICK_OUTER, GP_SET_F_PROFILE, 0, 30, 0, P(stick_outer), "stick_outer" },
    { K_STICK_ANTI, GP_SET_F_PROFILE, 0, 50, 0, P(stick_anti_deadzone), "stick_anti_deadzone" },
    { K_STICK_CURVE, GP_SET_F_PROFILE, 0, 3, 0, P(stick_curve), "stick_curve" },
    { K_TRIG_DZ, GP_SET_F_PROFILE, 0, 50, 0, P(trigger_deadzone), "trigger_deadzone" },
    { K_TRIG_OUTER, GP_SET_F_PROFILE, 0, 30, 0, P(trigger_outer), "trigger_outer" },
    { K_HAIR, GP_SET_F_PROFILE, 0, 1, 0, P(hair_trigger), "hair_trigger" },
    { K_TURBO_RATE, GP_SET_F_PROFILE, 1, 30, 10, P(turbo_rate), "turbo_rate" },
};
#define N_FIXED     ARRAY_SIZE(fixed)

struct gp_settings gp_settings;
static volatile u8 gp_dirty;
static volatile u16 gp_dirty_ms;

int gp_settings_count(void)
{
    return N_FIXED + 2 * GP_MAP_N;
}

int gp_settings_desc(int index, struct gp_setting_desc *d)
{
    memset(d, 0, sizeof(*d));
    if (index < 0) {
        return 0;
    }
    if (index < N_FIXED) {
        const struct fixed *f = &fixed[index];
        d->key = f->key;
        d->flags = f->flags;
        d->min = f->min;
        d->max = f->max;
        d->def = f->def;
        strncpy(d->name, f->name, sizeof(d->name) - 1);
        return 1;
    }
    index -= N_FIXED;
    if (index < GP_MAP_N) {
        d->key = K_MAP + index;
        d->flags = GP_SET_F_PROFILE;
        d->min = 0;
        d->max = GP_MAP_NONE;
        d->def = index;                                  /* each button as itself */
        snprintf(d->name, sizeof(d->name), "map_%s", gp_map_names[index]);
        return 1;
    }
    index -= GP_MAP_N;
    if (index < GP_MAP_N) {
        d->key = K_TURBO + index;
        d->flags = GP_SET_F_PROFILE;
        d->min = 0;
        d->max = 1;
        d->def = 0;
        snprintf(d->name, sizeof(d->name), "turbo_%s", gp_map_names[index]);
        return 1;
    }
    return 0;
}

int gp_settings_find(u8 key, struct gp_setting_desc *d)
{
    for (int i = 0; i < gp_settings_count(); i++) {
        if (gp_settings_desc(i, d) && d->key == key) {
            return 1;
        }
    }
    return 0;
}

u8 *gp_settings_ptr(u8 key, int profile)
{
    struct gp_profile *p = &gp_settings.prof[profile < 0 || profile >= GP_PROFILES ? 0 : profile];
    if (key >= K_MAP && key < K_MAP + GP_MAP_N) {
        return &p->map[key - K_MAP];
    }
    if (key >= K_TURBO && key < K_TURBO + GP_MAP_N) {
        return &p->turbo[key - K_TURBO];
    }
    for (int i = 0; i < N_FIXED; i++) {
        if (fixed[i].key == key) {
            return (fixed[i].flags & GP_SET_F_PROFILE ? (u8 *)p : (u8 *)&gp_settings) + fixed[i].off;
        }
    }
    return NULL;
}

/* Every setting of every profile to its default; restart ones (the mode)
 * only if asked (the config protocol defers those to a restart). */
void gp_settings_set_defaults(int restart_ones)
{
    struct gp_setting_desc d;
    for (int i = 0; gp_settings_desc(i, &d); i++) {
        if ((d.flags & GP_SET_F_RESTART) && !restart_ones) {
            continue;
        }
        for (int p = 0; p < (d.flags & GP_SET_F_PROFILE ? GP_PROFILES : 1); p++) {
            *gp_settings_ptr(d.key, p) = d.def;
        }
    }
}

/* Out of range (a damaged record, or one from a build with other ranges):
 * that setting's default. */
static void validate(void)
{
    struct gp_setting_desc d;
    for (int i = 0; gp_settings_desc(i, &d); i++) {
        for (int p = 0; p < (d.flags & GP_SET_F_PROFILE ? GP_PROFILES : 1); p++) {
            u8 *v = gp_settings_ptr(d.key, p);
            if (*v < d.min || *v > d.max) {
                *v = d.def;
            }
        }
    }
}

void gp_settings_load(void)
{
    gp_settings_set_defaults(1);
    u8 r[2 + sizeof(struct gp_settings)];
    memset(r, 0, sizeof(r));
    int n = gp_store_read(GP_CFG_ID, r, sizeof(r));
    if (n < 3 || r[0] != GP_CFG_MAGIC || r[1] < 1 || r[1] > GP_CFG_VERSION) {
        return;
    }
    if (r[1] == GP_CFG_VERSION) {
        /* The struct as stored; settings appended since load as defaults. */
        memcpy(&gp_settings, r + 2, MIN(n - 2, (int)sizeof(gp_settings)));
    } else {
        /* Versions 1-3: magic, version, rumble, mode (2+), idle-off (3+). */
        gp_settings.rumble_level = r[2];
        if (r[1] >= 2 && n >= 4) {
            gp_settings.mode = r[3];
        }
        if (r[1] >= 3 && n >= 5) {
            gp_settings.idle_off_min = r[4];
        }
    }
    validate();
}

static void gp_settings_save(void)
{
    u8 r[2 + sizeof(struct gp_settings)];
    r[0] = GP_CFG_MAGIC;
    r[1] = GP_CFG_VERSION;
    memcpy(r + 2, &gp_settings, sizeof(gp_settings));
    gp_dirty = 0;
    int n = gp_store_write(GP_CFG_ID, r, sizeof(r));
    if (n != sizeof(r)) {
        gp_log("settings: save failed (%d of %d bytes)", n, (int)sizeof(r));
    }
}

void gp_settings_changed(void)
{
    gp_dirty = 1;
    gp_dirty_ms = 0;
}

void gp_settings_poll(void)
{
    if (gp_dirty && (gp_dirty_ms += GP_POLL_MS) >= GP_SAVE_DELAY_MS) {
        gp_dirty_ms = 0;
        gp_settings_save();
    }
}

void gp_settings_flush(void)
{
    if (gp_dirty) {
        gp_settings_save();
    }
}
