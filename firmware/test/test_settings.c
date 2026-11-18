/* test_settings.c: the settings table, load / save, old records
 * (core/gp_settings.c). */
#include "test.h"
#include "gamepad.h"
#include "gp_settings.h"
#include "gp_store.h"

static void table_consistent(void)
{
    struct gp_setting_desc d, e;
    int n = gp_settings_count();
    CHECK_EQ(n, 13 + 2 * GP_MAP_N);
    for (int i = 0; i < n; i++) {
        CHECK(gp_settings_desc(i, &d));
        CHECK(d.min <= d.def && d.def <= d.max);         /* the default is in range */
        CHECK(d.name[0]);
        CHECK(gp_settings_ptr(d.key, 0) != NULL);
        CHECK(gp_settings_find(d.key, &e) && e.key == d.key);
        for (int j = i + 1; j < n; j++) {
            gp_settings_desc(j, &e);
            CHECK(e.key != d.key);                       /* keys unique: clients store them */
            CHECK(strcmp(e.name, d.name));
        }
    }
    CHECK(!gp_settings_desc(n, &d));
    CHECK(!gp_settings_desc(-1, &d));
    CHECK(!gp_settings_find(0xff, &d));
    CHECK(gp_settings_ptr(0xff, 0) == NULL);
}

static void stable_keys(void)
{
    /* Keys are part of the protocol (docs/config-protocol.md): they must not move. */
    struct gp_setting_desc d;
    CHECK(gp_settings_find(1, &d) && !strcmp(d.name, "rumble_level"));
    CHECK(gp_settings_find(2, &d) && !strcmp(d.name, "mode") && (d.flags & GP_SET_F_RESTART));
    CHECK(gp_settings_find(5, &d) && !strcmp(d.name, "link"));
    CHECK(gp_settings_find(0x10, &d) && !strcmp(d.name, "stick_deadzone") && d.def == 4);
    CHECK(gp_settings_find(0x40, &d) && !strcmp(d.name, "map_a"));
    CHECK(gp_settings_find(0x40 + GP_MAP_N - 1, &d) && !strcmp(d.name, "map_capture"));
    CHECK(gp_settings_find(0x60, &d) && !strcmp(d.name, "turbo_a"));
}

static void per_profile_pointers(void)
{
    /* Per-profile keys point into each profile; global ones don't move. */
    CHECK(gp_settings_ptr(0x10, 0) == &gp_settings.prof[0].stick_deadzone);
    CHECK(gp_settings_ptr(0x10, 3) == &gp_settings.prof[3].stick_deadzone);
    CHECK(gp_settings_ptr(0x40 + 2, 1) == &gp_settings.prof[1].map[2]);
    CHECK(gp_settings_ptr(1, 2) == &gp_settings.rumble_level);
    CHECK(gp_settings_ptr(0x10, 99) == &gp_settings.prof[0].stick_deadzone);    /* out of range: profile 0 */
}

static void defaults(void)
{
    memset(&gp_settings, 0x77, sizeof(gp_settings));
    gp_settings_set_defaults(1);
    CHECK_EQ(gp_settings.rumble_level, 4);
    CHECK_EQ(gp_settings.mode, GP_MODE_STEAM);
    CHECK_EQ(gp_settings.idle_off_min, 10);
    CHECK_EQ(gp_settings.link, GP_LINK_BT);
    for (int p = 0; p < GP_PROFILES; p++) {
        CHECK_EQ(gp_settings.prof[p].stick_deadzone, 4);
        for (int i = 0; i < GP_MAP_N; i++) {
            CHECK_EQ(gp_settings.prof[p].map[i], i);     /* each button as itself */
            CHECK_EQ(gp_settings.prof[p].turbo[i], 0);
        }
    }
    gp_settings.mode = GP_MODE_XBOX;
    gp_settings_set_defaults(0);                         /* restart-flagged ones kept */
    CHECK_EQ(gp_settings.mode, GP_MODE_XBOX);
}

static void load_nothing_stored(void)
{
    fake_store_clear();
    memset(&gp_settings, 0x77, sizeof(gp_settings));
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 4);
    CHECK_EQ(gp_settings.prof[2].map[5], 5);
}

static void save_after_delay_and_round_trip(void)
{
    fake_store_clear();
    gp_settings_set_defaults(1);
    gp_settings.rumble_level = 1;
    gp_settings.prof[3].stick_curve = 2;
    gp_settings.prof[1].map[0] = GP_MAP_NONE;
    gp_settings.link = GP_LINK_RECEIVER;
    gp_settings_changed();
    u8 r[2 + sizeof(struct gp_settings)];
    for (int ms = GP_POLL_MS; ms < 2000; ms += GP_POLL_MS) {
        gp_settings_poll();
    }
    CHECK_EQ(gp_store_read(GP_STORE_SETTINGS, r, sizeof(r)), -1);  /* not before 2 s */
    gp_settings_poll();
    CHECK_EQ(gp_store_read(GP_STORE_SETTINGS, r, sizeof(r)), sizeof(r));
    gp_settings_poll();                                  /* nothing changed since: no second write */

    gp_settings_set_defaults(1);
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 1);
    CHECK_EQ(gp_settings.prof[3].stick_curve, 2);
    CHECK_EQ(gp_settings.prof[1].map[0], GP_MAP_NONE);
    CHECK_EQ(gp_settings.link, GP_LINK_RECEIVER);
}

static void flush_saves_now(void)
{
    fake_store_clear();
    gp_settings_set_defaults(1);
    gp_settings_flush();                                 /* nothing changed: nothing written */
    u8 r[2 + sizeof(struct gp_settings)];
    CHECK_EQ(gp_store_read(GP_STORE_SETTINGS, r, sizeof(r)), -1);
    gp_settings_changed();
    gp_settings_flush();                                 /* power-off: at once */
    CHECK_EQ(gp_store_read(GP_STORE_SETTINGS, r, sizeof(r)), sizeof(r));
}

static void save_failure_logged(void)
{
    fake_store_clear();
    fake_store_fail = 1;
    fake_log_clear();
    gp_settings_changed();
    gp_settings_flush();
    CHECK(fake_log_has("settings: save failed"));
    fake_store_fail = 0;
}

static void out_of_range_value_gets_default(void)
{
    fake_store_clear();
    u8 r[2 + sizeof(struct gp_settings)];
    gp_settings_set_defaults(1);
    r[0] = 0xC2;
    r[1] = 4;
    memcpy(r + 2, &gp_settings, sizeof(gp_settings));
    struct gp_settings *s = (struct gp_settings *)(r + 2);
    s->rumble_level = 9;                                 /* max 4 */
    s->prof[2].stick_deadzone = 200;                     /* max 50 */
    s->prof[0].map[3] = GP_MAP_N + 5;                    /* past "none" */
    s->prof[0].map[4] = GP_MAP_NONE;                     /* "none" itself is fine */
    fake_store_put(GP_STORE_SETTINGS, r, sizeof(r));
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 4);
    CHECK_EQ(gp_settings.prof[2].stick_deadzone, 4);
    CHECK_EQ(gp_settings.prof[0].map[3], 3);
    CHECK_EQ(gp_settings.prof[0].map[4], GP_MAP_NONE);
}

static void shorter_record_loads_new_fields_as_defaults(void)
{
    /* A record from before "link" was appended: everything up to it loads,
     * link is its default. */
    fake_store_clear();
    gp_settings_set_defaults(1);
    gp_settings.rumble_level = 2;
    u8 r[2 + sizeof(struct gp_settings)];
    r[0] = 0xC2;
    r[1] = 4;
    memcpy(r + 2, &gp_settings, sizeof(gp_settings));
    r[2 + offsetof(struct gp_settings, link)] = GP_LINK_RECEIVER;
    fake_store_put(GP_STORE_SETTINGS, r, 2 + offsetof(struct gp_settings, link));
    gp_settings.link = GP_LINK_RECEIVER;
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 2);
    CHECK_EQ(gp_settings.link, GP_LINK_BT);
}

static void old_versions(void)
{
    fake_store_clear();
    const u8 v1[] = { 0xC2, 1, 3 };
    fake_store_put(GP_STORE_SETTINGS, v1, sizeof(v1));
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 3);
    CHECK_EQ(gp_settings.mode, GP_MODE_STEAM);

    const u8 v3[] = { 0xC2, 3, 0, GP_MODE_XBOX, 25 };
    fake_store_put(GP_STORE_SETTINGS, v3, sizeof(v3));
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 0);
    CHECK_EQ(gp_settings.mode, GP_MODE_XBOX);
    CHECK_EQ(gp_settings.idle_off_min, 25);
    CHECK_EQ(gp_settings.prof[0].stick_deadzone, 4);     /* profiles: defaults */

    const u8 bad_magic[] = { 0x00, 4, 1, 1, 1 };
    fake_store_put(GP_STORE_SETTINGS, bad_magic, sizeof(bad_magic));
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 4);               /* ignored: defaults */
    const u8 future[] = { 0xC2, 99, 1, 1, 1 };
    fake_store_put(GP_STORE_SETTINGS, future, sizeof(future));
    gp_settings_load();
    CHECK_EQ(gp_settings.rumble_level, 4);
}

int main(void)
{
    printf("test_settings\n");
    RUN(table_consistent);
    RUN(stable_keys);
    RUN(per_profile_pointers);
    RUN(defaults);
    RUN(load_nothing_stored);
    RUN(save_after_delay_and_round_trip);
    RUN(flush_saves_now);
    RUN(save_failure_logged);
    RUN(out_of_range_value_gets_default);
    RUN(shorter_record_loads_new_fields_as_defaults);
    RUN(old_versions);
    return t_done("test_settings");
}
