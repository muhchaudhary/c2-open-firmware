/* gp_core.c: see gp_core.h. */
#include "system/includes.h"
#include "os/os_api.h"
#include "asm/power/p33.h"
#include "gamepad.h"
#include "gp_core.h"
#include "gp_transport.h"
#include "gp_bt.h"
#include "gp_usb.h"
#include "gp_sinput.h"
#include "gp_xinput.h"
#include "gp_xboxbt.h"
#include "gp_ds4.h"
#include "gp_link.h"
#include "gp_leds.h"
#include "gp_rumble.h"
#include "gp_settings.h"
#include "gp_config.h"
#include "gp_cal.h"
#include "gp_tuning.h"
#include "gp_power.h"
#include "gp_log.h"

extern void wdt_clear(void);
extern const u8 *bt_get_mac_addr(void);

/* An unasked features reply every FEAT_UNASKED_MS while connected, as the
 * hybrid did (docs/sinput.md 2.7). SDL waits only ~100 ms for the reply to
 * its request, and over Bluetooth the asked one reached hidraw 157-195 ms
 * after the request was written (2026-11-12, scratchpad featprobe: the
 * firmware answers 2-11 ms after it sees the request; the rest is the
 * request's way in). Without one in that window SDL took the generic
 * driver: no d-pad mapping, no gyro. This used to stop 10 s after
 * connecting, so an app opened later never got SInput. SDL ignores report
 * 2 outside its init; the cost is one state report per 40 ms. */
#define FEAT_UNASKED_MS     40
/* For the first FEAT_EARLY_MS after connecting, one every 5 ticks instead
 * (as before): SDL opens the pad right at the connect, while the TX pool
 * is full most of the time, and every 40 ms wasn't enough there
 * (2026-11-12: generic driver at the connect, SInput on reopening). */
#define FEAT_EARLY_MS       10000

static u16 gp_si_timer;                 /* the report tick, stopped at power-off */
static u16 gp_conn_ticks;               /* ticks since connecting (the early features cadence) ... */
static u32 gp_conn_t0;                  /* ... and since when (gp_ms) */
static u32 gp_ts_us;
static volatile u32 gp_app_beat;        /* the poll ran (the stall detector) */

/* ---- Stall detector, in the tick interrupt. The watchdog (4 s) is cleared
 * only from the idle task, and the BT stack's task once stalled for 14 s,
 * 19 s or for good (the command-ring race, see gp_bt_send_posted; fixed). If
 * the app task's poll (gp_app_beat) stops: log it with the task that is
 * running instead, stop rumble (the host's "off" can't get through), and
 * keep the watchdog fed for up to STALL_RIDE_MS to let it end by itself (2
 * of 3 did); past that the watchdog resets. */
#define STALL_RIDE_MS       30000

static void gp_stall_check(void)
{
    static u32 last_beat, beat_t0, next_log;
    static u8 started, stopped;
    u32 now = gp_ms();
    if (!started || gp_app_beat != last_beat) {
        u32 ms = now - beat_t0;
        if (started && ms >= 500) {
            gp_log("stall over after %u ms", (unsigned)ms);
        }
        started = 1;
        last_beat = gp_app_beat;
        beat_t0 = now;
        stopped = 0;
        next_log = 500;
        return;
    }
    u32 ms = now - beat_t0;
    if (ms >= 1000 && ms < STALL_RIDE_MS) {
        wdt_clear();
    }
    if (ms >= 500 && !stopped) {
        stopped = 1;
        gp_rumble_stop();
    }
    if (ms >= next_log) {                                /* at 0.5 s, then every 5 s */
        next_log = next_log < 5000 ? 5000 : next_log + 5000;
        const char *cur = os_current_task();
        gp_log("STALL: app task silent %u ms, \"%s\" running", (unsigned)ms, cur ? cur : "?");
    }
}

/* ---- Low battery: on battery at GP_LOWBAT_PCT or less (the board's
 * measured table: ~1 h idle before the power-off), Home flashes red until
 * 5 V comes (gp_leds_low_battery). The percent only falls on battery, so it
 * doesn't flicker; the SDK's own 3.4 V warning only logs. */
#define GP_LOWBAT_PCT       5
#define GP_LOWBAT_TEST_MS   10000
static volatile u8 gp_lowbat_test;                      /* log command "lowbat test": show it for a while */

void gp_core_lowbat_test(void)
{
    gp_lowbat_test = 1;
}

static void gp_low_battery_poll(int connected)
{
    static u8 low;
    static u32 test_t0;
    static u8 testing;
    if (gp_lowbat_test) {
        gp_lowbat_test = 0;
        testing = 1;
        test_t0 = gp_ms();
    }
    if (testing && gp_ms() - test_t0 >= GP_LOWBAT_TEST_MS) {
        testing = 0;
    }
    struct gp_battery b;
    gp_board_battery(&b);
    u8 now = testing || (b.plug == 4 && b.percent <= GP_LOWBAT_PCT);
    if (now != low) {
        low = now;
        gp_log("battery: %s (%u %%, %u mV)", low ? "low: Home flashes red" : "no longer low",
               b.percent, b.mv);
    }
    gp_leds_low_battery(low, connected);
}

/* ---- The report tick ------------------------------------------------------ */

/* Buttons as the host sees them. The D-pad is hidden while the modifier is
 * held (Modifier + Up / Down: rumble level; Left / Right: profile). Then
 * the active profile's remap and turbo: each physical button gives the
 * button it maps to (or none), a turbo one only every other half period of
 * turbo_rate. The pad's own combos read the raw buttons, so a remap never
 * breaks them. */
static u32 gp_host_buttons(u32 raw)
{
    if (raw & GP_BIT(GP_BTN_MODIFIER)) {
        raw &= ~(GP_BIT(GP_BTN_UP) | GP_BIT(GP_BTN_DOWN) | GP_BIT(GP_BTN_LEFT) | GP_BIT(GP_BTN_RIGHT));
    }
    const struct gp_profile *p = &GP_ACTIVE_PROFILE;
    u32 now = jiffies_msec() % 60000;
    int turbo_off = (now * p->turbo_rate * 2 / 1000) & 1;
    u32 out = 0, mapped = 0;
    for (int i = 0; i < GP_MAP_N; i++) {
        u32 bit = GP_BIT(gp_map_buttons[i]);
        mapped |= bit;
        if (!(raw & bit) || (p->turbo[i] && turbo_off)) {
            continue;
        }
        if (p->map[i] < GP_MAP_N) {
            out |= GP_BIT(gp_map_buttons[p->map[i]]);
        }
    }
    return (out | (raw & ~mapped)) & GP_REPORTED_BUTTONS;
}

/* Modifier + Left / Right: the previous / next profile, shown on the player
 * LEDs for a moment; saved like any setting. */
static void gp_profile_combo(void)
{
    static u8 last;
    u32 b = gp_board_buttons();
    u8 now = 0;
    if (b & GP_BIT(GP_BTN_MODIFIER)) {
        now = ((b & GP_BIT(GP_BTN_LEFT)) ? 1 : 0) | ((b & GP_BIT(GP_BTN_RIGHT)) ? 2 : 0);
    }
    u8 pressed = now & ~last;
    last = now;
    if (!(pressed & 3)) {
        return;
    }
    u8 p = gp_settings.profile;
    p = pressed & 2 ? (p + 1) % GP_PROFILES : (p + GP_PROFILES - 1) % GP_PROFILES;
    gp_settings.profile = p;
    gp_settings_changed();
    gp_leds_show_profile();
    gp_log("profile %u", p + 1);
}

static void gp_read_state(struct gp_state *s, u32 raw_buttons)
{
    memset(s, 0, sizeof(*s));
    s->buttons = gp_host_buttons(raw_buttons);
    gp_board_read(s);
    gp_cal_tick();
    gp_tune_stick(&s->lx, &s->ly);                       /* the active profile's shaping */
    gp_tune_stick(&s->rx, &s->ry);
    gp_tune_trigger(&s->lt);
    gp_tune_trigger(&s->rt);
    if (s->lx | s->ly | s->rx | s->ry ||                 /* sticks (after the dead-zone) */
        (s->lt >> 8) > -120 || (s->rt >> 8) > -120) {    /* triggers off rest */
        gp_power_activity();
    }
}

static void gp_build_state(u8 *rep, u32 raw_buttons, const struct gp_battery *b)
{
    struct gp_state s;
    gp_read_state(&s, raw_buttons);
    sinput_encode_state(rep, &s, b, gp_ts_us);
}

static void gp_build_features(u8 *rep)
{
    const u8 *mac = bt_get_mac_addr();
    u8 serial[6];
    for (int k = 0; k < 6; k++) {                        /* the BT address, MSB first */
        serial[k] = mac[5 - k];
    }
    sinput_encode_features(rep, &gp_board_info.sinput, serial);
}

/* The active transport: USB while a host has the gamepad open on the cable,
 * else Bluetooth. */
static const struct gp_transport *gp_transport(void)
{
    return gp_usb_host_ready() ? &gp_usb_transport : &gp_bt_transport;
}

static void gp_tick_xboxbt(const struct gp_transport *tr, u32 buttons);
static void gp_tick_ds4(const struct gp_transport *tr, u32 buttons, int bt);
static volatile u8 gp_link_hello_due = 1, gp_link_status_due = 1;   /* receiver link (gp_tick_link) */
static void gp_tick_link(const struct gp_transport *tr, u32 buttons, u32 now_ms);

static void gp_tick_body(void)
{
    static const struct gp_transport *last_tr;
    const struct gp_transport *gp_tr = gp_transport();
    if (gp_tr != last_tr) {
        gp_conn_ticks = 0;                               /* a new host: unasked features replies again */
        gp_conn_t0 = gp_ms();
        sinput_battery_reset();                          /* ... and the battery report */
        last_tr = gp_tr;
    }
    if (!gp_tr->connected()) {
        gp_conn_ticks = 0;
        gp_conn_t0 = gp_ms();
        sinput_battery_reset();
        gp_link_hello_due = 1;                           /* the next connection starts with a hello */
        gp_link_status_due = 1;
        gp_rumble_stop();                                /* no host: no rumble */
        return;
    }
    /* SDL integrates the gyro on this timestamp, so it must follow real
     * time: the usr_timer's period wanders with load (debug log,
     * 2026-11-13: 6.4 ms idle, 7.7 ms connected for a 6 ms request). Taken
     * from the system's millisecond clock (it kept to the PC's within
     * 0.2 s a minute); 1 ms steps, but the sum of the steps is exact. */
    static u32 last_ms;
    u32 now_ms = jiffies_msec();
    if (last_ms) {
        gp_ts_us += (now_ms - last_ms) * 1000;
    }
    last_ms = now_ms;
    u32 buttons = gp_board_buttons();
    gp_rumble_tick(buttons);
    gp_board_tick();
    if (gp_conn_ticks < 0xffff) {
        gp_conn_ticks++;
    }
    static u32 feat_ms;                                  /* when the last features reply was built */
    int early = now_ms - gp_conn_t0 < FEAT_EARLY_MS;
    int unasked = sinput_feat_retry || (early ? gp_conn_ticks % 5 == 0 : now_ms - feat_ms >= FEAT_UNASKED_MS);
    /* One report in flight at most: while the last one is still being sent,
     * skip this tick, so the next report carries the newest state instead of
     * queueing stale ones behind it (a full queue also dropped presses and
     * releases). A pending features reply waits for the next free tick. A
     * report the transport hasn't sent yet is simply replaced. */
    if (gp_tr->busy()) {
        return;
    }
    if (gp_tr->protocol() == GP_PROTO_XINPUT) {
        struct gp_state s;
        u8 x[XINPUT_REPORT_LEN];
        gp_read_state(&s, buttons);
        xinput_encode_state(x, &s);
        gp_tr->post(0, x, sizeof(x));
        return;
    }
    if (gp_tr->protocol() == GP_PROTO_XBOXBT) {
        gp_tick_xboxbt(gp_tr, buttons);
        return;
    }
    if (gp_tr->protocol() == GP_PROTO_LINK) {
        gp_tick_link(gp_tr, buttons, now_ms);
        return;
    }
    if (gp_tr->protocol() == GP_PROTO_DS4 || gp_tr->protocol() == GP_PROTO_DS4BT) {
        gp_tick_ds4(gp_tr, buttons, gp_tr->protocol() == GP_PROTO_DS4BT);
        return;
    }
    u8 rep[SINPUT_REPORT_LEN];
    u8 id;
    struct gp_battery b;
    gp_board_battery(&b);                                /* every tick: the average and the "full" hold */
    if (!sinput_feat_pending && gp_config_reply(rep)) {   /* a configuration reply (gp_config.h) */
        gp_tr->post(SINPUT_ID_FEATURES, rep, sizeof(rep));
        return;
    }
    if (sinput_feat_pending || unasked) {
        if (sinput_feat_pending) {
            sinput_feat_asked = 1;
        }
        sinput_feat_pending = 0;
        sinput_feat_retry = 0;
        feat_ms = now_ms;
        gp_build_features(rep);
        id = SINPUT_ID_FEATURES;
    } else if (!early && sinput_battery_due(&b, now_ms)) {
        sinput_encode_battery(rep, &b);                  /* Linux's battery (gp_sinput_desc.c), after the handshake */
        id = SINPUT_ID_BATTERY;
    } else {
        gp_build_state(rep, buttons, &b);
        id = SINPUT_ID_STATE;
    }
    gp_tr->post(id, rep, sizeof(rep));
}

/* Xbox One S over Bluetooth: the state report every tick; the Xbox button
 * as report 2 when it changes (until the transport has sent it), and the
 * battery as report 4 now and then (as v19 did: once per 256 reports). */
static void gp_tick_xboxbt(const struct gp_transport *tr, u32 buttons)
{
    static u8 guide_last;
    static u8 count;
    struct gp_state s;
    struct gp_battery b;
    u8 rep[XBOXBT_REPORT_LEN];
    gp_board_battery(&b);                                /* every tick: the average and the "full" hold */
    gp_read_state(&s, buttons);
    u8 guide = (s.buttons & GP_BIT(GP_BTN_GUIDE)) != 0;
    if (guide != guide_last) {
        guide_last = guide;
        xboxbt_guide_dirty = 1;
    }
    if (xboxbt_guide_dirty) {
        rep[0] = guide;
        tr->post(XBOXBT_ID_GUIDE, rep, 1);
        return;
    }
    if (++count == 0) {
        rep[0] = b.percent * 255 / 100;
        tr->post(XBOXBT_ID_BATTERY, rep, 1);
        return;
    }
    xboxbt_encode_state(rep, &s);
    tr->post(XBOXBT_ID_STATE, rep, sizeof(rep));
}

/* DS4 (gp_ds4.h): the state every tick, battery and IMU in it; USB 0x01,
 * Bluetooth 0x11 (with its CRC). The feature reports go on request, outside
 * the tick (GET_REPORT: gp_usb.c, gp_bt.c). */
static void gp_tick_ds4(const struct gp_transport *tr, u32 buttons, int bt)
{
    static u8 counter;
    struct gp_state s;
    struct gp_battery b;
    u8 rep[DS4_BT_REPORT_LEN];
    gp_board_battery(&b);                                /* every tick: the average and the "full" hold */
    gp_read_state(&s, buttons);
    if (bt) {
        ds4_encode_bt(rep, &s, &b, gp_ts_us, counter++);
        tr->post(DS4_ID_BT_STATE, rep, DS4_BT_REPORT_LEN);
    } else {
        ds4_encode_usb(rep, &s, &b, gp_ts_us, counter++);
        tr->post(DS4_ID_USB_STATE, rep, DS4_USB_REPORT_LEN);
    }
}

/* The receiver link (gp_link.h): a configuration reply first (SInput 0x02,
 * as is), then a hello (at the connect, when asked, after a mode change),
 * then the status (battery: on a change and every GP_LINK_STATUS_MS),
 * else the state. No features replies: the receiver answers SInput's
 * features request itself, from the hello. */
void gp_core_link_hello(void)
{
    gp_link_hello_due = 1;
}

void gp_core_link_unsent(u8 id)
{
    if (id == GP_LINK_ID_HELLO) {
        gp_link_hello_due = 1;
    } else {
        gp_link_status_due = 1;
    }
}

static void gp_tick_link(const struct gp_transport *tr, u32 buttons, u32 now_ms)
{
    static u32 status_ms;
    static struct gp_battery last;
    struct gp_battery b;
    u8 rep[SINPUT_REPORT_LEN];
    gp_board_battery(&b);                                /* every tick: the average and the "full" hold */
    if (b.plug != last.plug || b.percent != last.percent || b.charging != last.charging ||
        now_ms - status_ms >= GP_LINK_STATUS_MS) {
        gp_link_status_due = 1;
    }
    if (gp_config_reply(rep)) {                          /* a configuration reply (gp_config.h) */
        tr->post(SINPUT_ID_FEATURES, rep, SINPUT_REPORT_LEN);
        return;
    }
    if (gp_link_hello_due) {
        struct gp_link_hello h;
        const u8 *mac = bt_get_mac_addr();
        gp_link_hello_due = 0;
        h.version = GP_LINK_VERSION;
        h.mode = gp_settings.mode;
        h.caps = gp_board_info.sinput;
        for (int k = 0; k < 6; k++) {                    /* the BT address, MSB first (as the features reply) */
            h.serial[k] = mac[5 - k];
        }
        h.tick_us = GP_TICK_MS * 1000;
        gp_link_encode_hello(rep, &h);
        tr->post(GP_LINK_ID_HELLO, rep, GP_LINK_HELLO_LEN);
        return;
    }
    if (gp_link_status_due) {
        gp_link_status_due = 0;
        status_ms = now_ms;
        last = b;
        gp_link_encode_status(rep, &b);
        tr->post(GP_LINK_ID_STATUS, rep, GP_LINK_STATUS_LEN);
        return;
    }
    struct gp_state s;
    gp_read_state(&s, buttons);
    gp_link_encode_state(rep, 0, &s, (u16)(gp_ts_us / 1000));   /* the sequence goes on as it is sent (gp_bt.c) */
    tr->post(GP_LINK_ID_STATE, rep, GP_LINK_STATE_LEN);
}

static void gp_tick(void *priv)
{
    gp_stall_check();
    gp_tick_body();
}

static void gp_core_tick_start(void)
{
    gp_si_timer = sys_s_hi_timer_add(NULL, gp_tick, GP_TICK_REQ_MS);
}

void gp_core_tick_stop(void)
{
    if (gp_si_timer) {
        sys_s_hi_timer_del(gp_si_timer);
        gp_si_timer = 0;
    }
}

/* ---- The poll (app task) ------------------------------------------------- */

/* Every 60 s a short status line; hardware-wait timeouts as they happen. */
static void gp_log_status(const char *tr, u8 connected)
{
    static u16 ms;
    static u8 hangs;
    static u32 sent, sfail;
    if (gp_hang_count != hangs) {
        gp_log("hang: a bounded hardware wait ran out (%u so far)", gp_hang_count);
        hangs = gp_hang_count;
    }
    if ((ms += GP_POLL_MS) < 60000) {
        return;
    }
    ms = 0;
    char board[96];
    board[0] = 0;
    gp_board_status(board, sizeof(board));
    gp_log("status: %s %u | bt sent %u, failed %u (last %d) | %s | rumble lvl %u",
           tr, connected, (unsigned)(gp_bt_sent - sent), (unsigned)(gp_hid_send_errs - sfail), gp_hid_send_err,
           board, gp_settings.rumble_level);
    sent = gp_bt_sent;
    sfail = gp_hid_send_errs;
}

/* Combos held MODE_HOLD_MS: Home + X = Xbox, Home + Y = Steam, Home + B =
 * DS4 (the input mode), Home + M = the link (Bluetooth <-> the C2 receiver, saved, then a
 * restart onto the other identity). A mode is saved; the pad then restarts
 * in the new mode (the Bluetooth
 * identity is set when the stack starts). On the receiver link the pad's
 * identity is the same in both modes, so no restart (and no reconnect or
 * pairing): the mode is saved and goes to the receiver, which changes what
 * it shows on USB. Except on the cable: the USB device is built for one
 * mode at enumeration (gp_usb.c), so wired it restarts as before. */
#define MODE_HOLD_MS        3000

static void gp_mode_restart(void *priv)
{
    p33_tx_1byte(P3_WKUP_CPND, 0xff);                    /* a normal boot, not "woken by USB" */
    cpu_reset();
}

/* Home's colour follows the mode: Steam white, Xbox green, DS4 blue. */
static void gp_core_mode_colour(void)
{
    static const u8 white[3] = { 16, 16, 16 };
    static const u8 green[3] = { 2, 16, 2 };            /* Xbox green, #107C10 at the LEDs' usual level */
    static const u8 blue[3] = { 0, 5, 16 };             /* PlayStation blue, #0070D1 */
    gp_leds_set_mode_colour(gp_settings.mode == GP_MODE_XBOX ? green : gp_settings.mode == GP_MODE_DS4 ? blue : white);
}

static const char *gp_mode_name(int mode)
{
    return mode == GP_MODE_XBOX ? "Xbox" : mode == GP_MODE_DS4 ? "DS4" : "Steam";
}

void gp_core_restart(const char *why)
{
    static u8 restarting;
    if (restarting) {
        return;
    }
    restarting = 1;
    gp_settings_flush();
    gp_log("restart: %s", why);
    gp_core_tick_stop();
    gp_rumble_stop();
    gp_leds_dark();
    if (gp_bt_link_up()) {
        gp_bt_disconnect();
    }
    sys_timeout_add(NULL, gp_mode_restart, 500);         /* the disconnect and the log go out first */
}

#define COMBO_LINK          0x7F                /* gp_mode_combo's want: the link, not a mode */

static void gp_mode_combo(void)
{
    static u16 held_ms;
    static s8 want_last = -1;
    static u8 switching;
    u32 b = gp_board_buttons();
    int want = -1;
    if (b & GP_BIT(GP_BTN_GUIDE)) {
        if (b & GP_BIT(GP_BTN_WEST)) {
            want = GP_MODE_XBOX;
        } else if (b & GP_BIT(GP_BTN_NORTH)) {
            want = GP_MODE_STEAM;
        } else if (b & GP_BIT(GP_BTN_EAST)) {
            want = GP_MODE_DS4;
        } else if (b & GP_BIT(GP_BTN_MODIFIER)) {
            want = COMBO_LINK;
        }
    }
    if (want < 0 || want != want_last) {
        held_ms = 0;
        want_last = want;
        return;
    }
    if (switching || held_ms >= MODE_HOLD_MS || (held_ms += GP_POLL_MS) < MODE_HOLD_MS) {
        return;
    }
    if (want == COMBO_LINK) {
        gp_settings.link = gp_bt_receiver() ? GP_LINK_BT : GP_LINK_RECEIVER;
        gp_settings_changed();
        switching = 1;
        gp_core_restart(gp_settings.link == GP_LINK_RECEIVER ? "link: the receiver" : "link: Bluetooth");
        return;
    }
    if (gp_settings.mode == want) {
        gp_log("mode: already %s", gp_mode_name(want));
        return;
    }
    gp_settings.mode = want;
    gp_settings_changed();
    if (gp_bt_receiver() && gp_transport() != &gp_usb_transport) {
        gp_log("mode: %s (receiver link: no restart)", gp_mode_name(want));
        gp_core_mode_colour();
        gp_leds_update();
        gp_core_link_hello();                            /* the hello carries the mode to the receiver */
        return;
    }
    switching = 1;
    gp_core_restart(want == GP_MODE_XBOX ? "mode Xbox" : want == GP_MODE_DS4 ? "mode DS4" : "mode Steam");
}

/* Home + Capture: tapped (released within PAIR_HOLD_MS) with a USB host on
 * the cable, USB input on / off (the gamepad on USB in the current mode, or
 * back to Bluetooth); tapped without one, nothing (the link is Home + M,
 * gp_mode_combo); held PAIR_HOLD_MS, pairing mode on the current link (as
 * stock). */
#define PAIR_HOLD_MS        3000
#define TAP_MIN_MS          100

/* DS4 mode: USB input waits for Bluetooth to be down. hid-playstation
 * reads the USB pad's address (feature 0x12, the Bluetooth one) at the
 * probe and refuses a second device with an address it has ("Duplicate
 * device found for MAC address", -EEXIST; 2026-11-17): the pad still on
 * Bluetooth then, as it stepped aside only once USB was up (gp_poll). So
 * Bluetooth goes first, then USB_SETTLE_MS for the host to drop its HID
 * device, USB_WAIT_MS at most. */
#define USB_SETTLE_MS       300
#define USB_WAIT_MS         3000

static u16 usb_wait_ms, usb_down_ms;                     /* usb_wait_ms: waiting */

static void gp_usb_input_wait(void)
{
    if (!usb_wait_ms) {
        return;
    }
    if (!gp_usb_host_present()) {                        /* the cable went: stay on Bluetooth */
        usb_wait_ms = 0;
        gp_bt_suspend(0);
        return;
    }
    usb_down_ms = gp_bt_link_up() ? 0 : usb_down_ms + GP_POLL_MS;
    usb_wait_ms += GP_POLL_MS;
    if (usb_down_ms < USB_SETTLE_MS && usb_wait_ms < USB_WAIT_MS) {
        return;
    }
    usb_wait_ms = 0;
    gp_usb_set_input(1);
    if (!gp_usb_input_on()) {                            /* refused (the log alone): back to Bluetooth */
        gp_bt_suspend(0);
    }
}

static void gp_home_capture(void)
{
    static u16 held_ms;
    const u32 combo = GP_BIT(GP_BTN_GUIDE) | GP_BIT(GP_BTN_CAPTURE);
    gp_usb_input_wait();
    if ((gp_board_buttons() & combo) == combo) {
        if (held_ms < PAIR_HOLD_MS && (held_ms += GP_POLL_MS) >= PAIR_HOLD_MS) {
            gp_bt_pairing_start();
        }
        return;
    }
    if (held_ms >= TAP_MIN_MS && held_ms < PAIR_HOLD_MS && gp_usb_host_present() && !usb_wait_ms) {
        if (!gp_usb_input_on() && gp_settings.mode == GP_MODE_DS4 && gp_bt_link_up()) {
            gp_log("usb: Bluetooth down first (DS4: one address)");
            gp_bt_suspend(1);
            usb_wait_ms = GP_POLL_MS;
            usb_down_ms = 0;
        } else {
            gp_usb_set_input(!gp_usb_input_on());
        }
    }
    held_ms = 0;
}

static void gp_poll(void *priv)
{
    static u8 was_connected, was_wired;
    gp_app_beat++;
    gp_board_poll();
    const struct gp_transport *gp_tr = gp_transport();
    u8 connected = gp_tr->connected() != 0;
    u8 wired = gp_tr == &gp_usb_transport;
    gp_board_dock_poll();
    if (gp_power_off_pending()) {
        gp_power_off_step();
        return;
    }
    /* Put on the charging dock: charging mode, as stock ("pedestal 5v
     * insert reset"). Only the step onto it counts: a pad started with Home
     * while docked stays on. The links close first (gp_power_off), then
     * the soft-off finds the dock's 5 V and starts charging mode. */
    static s8 was_docked = -1;
    int docked = gp_board_docked();
    if (docked >= 0) {
        if (was_docked == 0 && docked) {
            gp_power_off("docked");
            return;
        }
        was_docked = docked;
    }
    if (wired != was_wired) {
        gp_log("transport: %s", gp_tr->name);
        gp_bt_suspend(wired);                            /* wired: Bluetooth steps aside */
        was_wired = wired;
    }
    gp_bt_poll(gp_bt_transport.connected() != 0);
    gp_usb_poll();
    gp_log_status(gp_tr->name, connected);
    static u8 shown = 0xff;                              /* the link the player LEDs show */
    if (connected != was_connected) {
        gp_log("hid (%s): %s", gp_tr->name, connected ? "connected" : "disconnected");
        if (connected) {
            gp_leds_update();                            /* a host connected: back to its colours */
        }
        was_connected = connected;
    }
    u8 link = wired ? GP_LEDS_LINK_USB : gp_bt_receiver() ? GP_LEDS_LINK_RECEIVER : GP_LEDS_LINK_BT;
    if (connected && link != shown) {
        gp_leds_set_link(link);                          /* player LEDs: Bluetooth blue, USB white, receiver purple */
        shown = link;
    }
    gp_low_battery_poll(connected && !gp_power_home_held() && !gp_power_off_pending());   /* dark on purpose: leave it */
    if (!connected && !gp_power_home_held()) {
        gp_leds_home_anim(gp_bt_pairing());
    }
    /* Receiver link, not connected (pairing or reconnecting): all four
     * player LEDs purple, so the link shows before a connection (which
     * link the pad was on was hard to tell, 2026-11-13). Connected: the
     * player number in purple (gp_leds_set_link). */
    gp_leds_set_searching(gp_bt_receiver() && !connected);
    gp_mode_combo();
    gp_profile_combo();
    gp_home_capture();
    gp_leds_poll();
    gp_power_poll(connected, wired);
    gp_config_poll();
    gp_settings_poll();
}

void gp_core_start(void)
{
    gp_core_mode_colour();
    gp_board_start();
    gp_cal_load();                                       /* stored calibration, once the drivers are up */
    gp_rumble_init();
    if (!gp_bt_start()) {
        static const u8 red[3] = { 32, 0, 0 };           /* Home red: reports can't go out */
        gp_leds_set_host_rgb(red);
    }
    gp_core_tick_start();
    sys_timer_add(NULL, gp_poll, GP_POLL_MS);
}
