/*
 * gp_settings.h: settings kept across power-offs, one record in the
 * board's store (gp_store.h: syscfg item 49, or the C2's EEPROM). The store
 * must not run in an interrupt: changes are only marked
 * (gp_settings_changed, any context) and the poll writes them, 2 s after the
 * last change (one write per adjustment).
 *
 * Global settings, plus GP_PROFILES profiles (tuning, remap, turbo) of
 * which one is active. Every setting is a u8 described in a table
 * (gp_settings_desc): key, name, range, default, and whether it belongs to
 * the profile. The configuration protocol (gp_config.h) lists the table;
 * the record is the struct itself, so a setting appended later needs no
 * new record version (missing bytes load as defaults).
 */
#ifndef GP_SETTINGS_H
#define GP_SETTINGS_H

#include "typedef.h"

#define GP_PROFILES         4
#define GP_MAP_N            18          /* remappable buttons (gp_map_buttons) */
#define GP_MAP_NONE         GP_MAP_N    /* map target: no button */
#define GP_IDLE_OFF_MAX     60          /* minutes */

struct gp_profile {
    u8 stick_deadzone;                  /* % of full: inner dead zone (radial) */
    u8 stick_outer;                     /* %: full output this far before the edge */
    u8 stick_anti_deadzone;             /* %: output starts here once past the dead zone */
    u8 stick_curve;                     /* 0 linear, 1 ^1.5, 2 ^2, 3 ^3 */
    u8 trigger_deadzone;                /* % of travel */
    u8 trigger_outer;                   /* %: full this far before the end */
    u8 hair_trigger;                    /* 1: any press past the dead zone is full */
    u8 turbo_rate;                      /* presses per second */
    u8 map[GP_MAP_N];                   /* per physical button: the button the host gets (GP_MAP_NONE: none) */
    u8 turbo[GP_MAP_N];                 /* per physical button: 1 = turbo */
};

struct gp_settings {
    u8 rumble_level;                    /* 0 (off) .. 4 */
    u8 mode;                            /* enum gp_mode: Steam (SInput), Xbox, DS4 or Switch */
    u8 idle_off_min;                    /* connected and idle: off after this many minutes, 0 = never */
    u8 profile;                         /* the active profile, 0 .. GP_PROFILES - 1 */
    struct gp_profile prof[GP_PROFILES];
    u8 link;                            /* enum gp_link (appended: older saves load it as the default) */
};
extern struct gp_settings gp_settings;
#define GP_ACTIVE_PROFILE   (gp_settings.prof[gp_settings.profile])

/* The remappable buttons, in map index order (GP_BTN_*), and their names. */
extern const u8 gp_map_buttons[GP_MAP_N];
extern const char *const gp_map_names[GP_MAP_N];

/* ---- The table ------------------------------------------------------------ */
#define GP_SET_F_RESTART    0x01        /* takes effect after a restart */
#define GP_SET_F_PROFILE    0x02        /* per profile: get / set the active profile's */
struct gp_setting_desc {
    u8 key;
    u8 flags;
    u8 min, max, def;
    char name[24];
};
int gp_settings_count(void);
int gp_settings_desc(int index, struct gp_setting_desc *d);    /* 0 if index is past the end */
int gp_settings_find(u8 key, struct gp_setting_desc *d);       /* 0 if no such key */
u8 *gp_settings_ptr(u8 key, int profile);                      /* the value (profile: which one, for per-profile keys) */
void gp_settings_set_defaults(int restart_ones);               /* every setting (restart ones only if asked) */

void gp_settings_load(void);
void gp_settings_changed(void);
void gp_settings_poll(void);            /* every GP_POLL_MS */
void gp_settings_flush(void);           /* now, if anything changed (power-off) */

#endif
