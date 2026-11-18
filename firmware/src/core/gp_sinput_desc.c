/* gp_sinput_desc.c: the SInput report descriptor and the state / features
 * encoders, on their own so the C2 receiver (boards/dongle, which doesn't
 * build the pad's core) uses the same bytes. */
#include "typedef.h"
#include <string.h>
#include "gp_sinput.h"

/* The descriptor: the earlier SInput hybrid's (docs/sinput.md 2.3):
 * input 0x01 state (63 B), input 0x02 features (63 B), output 0x03 commands
 * (47 B), plus our input 0x04 battery (63 B), in place of the hybrid's
 * padding (five 0x64 items to its fixed 125-byte slot).
 *
 * Linux calls power_supply_changed() on *every* report carrying the Battery
 * System / Charging usage: in report 0x01 that was ~250 uevents a second,
 * more than udev handles, so its queue grew without end and new devices
 * waited behind it (root-only nodes, the receiver "not relaying",
 * 2026-11-13; still so in hid-input.c, 2026-11-15). So report 0x01's battery
 * bytes [2] (Battery Strength) and [6] bit 0 (Charging) are Constant
 * (81 03, the hybrid has 81 02): Linux ignores them, SDL reads the raw bytes
 * [1] / [2] as before. Linux takes both from report 0x04 instead, which is
 * sent only on a change and once a minute (sinput_battery_due): a few
 * uevents an hour. Linux keeps one battery per report ID; SDL and Steam
 * read report 0x01 only and drop other IDs. */
const u8 sinput_report_desc[SINPUT_REPORT_DESC_LEN] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01, 0x06, 0x00, 0xff, 0x09,
    0x20, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05,
    0x06, 0x09, 0x20, 0x81, 0x03, 0x05, 0x09, 0x19, 0x01, 0x29, 0x18, 0x25,
    0x01, 0x75, 0x01, 0x95, 0x18, 0x81, 0x02, 0x05, 0x85, 0x09, 0x44, 0x95,
    0x01, 0x81, 0x03, 0x95, 0x07, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09,
    0x31, 0x09, 0x33, 0x09, 0x34, 0x09, 0x32, 0x09, 0x35, 0x16, 0x00, 0x80,
    0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x06, 0x81, 0x02, 0x95, 0x02, 0x81,
    0x03, 0x09, 0x40, 0x09, 0x41, 0x09, 0x42, 0x09, 0x43, 0x09, 0x44, 0x09,
    0x45, 0x95, 0x06, 0x81, 0x02, 0x75, 0x08, 0x95, 0x1d, 0x81, 0x03, 0x85,
    0x02, 0x95, 0x3f, 0x81, 0x03, 0x85, 0x03, 0x95, 0x2f, 0x91, 0x03,
    /* input 0x04 (63 B): [1] Battery Strength 0..100, [2] bit 0 Charging */
    0x85, 0x04,
    0x05, 0x06, 0x09, 0x20, 0x15, 0x00, 0x25, 0x64, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x85, 0x09, 0x44, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x03,                              /* [2] bits 1..7 */
    0x75, 0x08, 0x95, 0x3d, 0x81, 0x03,                  /* [3..63] */
    0xc0
};

/* k: byte index as in docs/sinput.md (1 = right after the report ID) */
static void put16(u8 *rep, int k, int v)
{
    rep[k - 1] = (u8)v;
    rep[k] = (u8)(v >> 8);
}

void sinput_encode_state(u8 *rep, const struct gp_state *s, const struct gp_battery *b, u32 ts_us)
{
    memset(rep, 0, SINPUT_REPORT_LEN);
    rep[0] = b->plug;                                    /* [1] plug status */
    rep[1] = b->percent;                                 /* [2] battery % */
    rep[2] = (u8)s->buttons;                             /* [3..5] buttons */
    rep[3] = (u8)(s->buttons >> 8);
    rep[4] = (u8)(s->buttons >> 16);
    rep[5] = b->charging;                                /* [6] bit 0: charging */
    put16(rep, 7, s->lx);                                /* [7..14] sticks LX LY RX RY */
    put16(rep, 9, s->ly);
    put16(rep, 11, s->rx);
    put16(rep, 13, s->ry);
    put16(rep, 15, s->lt);                               /* [15..18] triggers LT, RT */
    put16(rep, 17, s->rt);
    put16(rep, 19, ts_us);                               /* [19..22] IMU timestamp, us */
    put16(rep, 21, ts_us >> 16);
    for (int i = 0; i < 6; i++) {                        /* [23..34]: accel X Y Z, gyro X Y Z */
        put16(rep, 23 + 2 * i, s->imu[i]);
    }
}

/* Report 0x04. Charging = on 5 V, full or not: Linux's HID battery knows
 * only Charging / Discharging (no Full), and a pad on the cable at 100 %
 * reading "Discharging" is the worse lie. */
static u8 bat_sent_pct = 0xff, bat_sent_chg;
static u32 bat_sent_ms;

void sinput_battery_reset(void)
{
    bat_sent_pct = 0xff;
}

int sinput_battery_due(const struct gp_battery *b, u32 now_ms)
{
    u8 chg = b->plug != 4;
    if (bat_sent_pct != 0xff && b->percent == bat_sent_pct && chg == bat_sent_chg &&
        now_ms - bat_sent_ms < SINPUT_BATTERY_MS) {
        return 0;
    }
    bat_sent_pct = b->percent;
    bat_sent_chg = chg;
    bat_sent_ms = now_ms;
    return 1;
}

void sinput_encode_battery(u8 *rep, const struct gp_battery *b)
{
    memset(rep, 0, SINPUT_REPORT_LEN);
    rep[0] = b->percent;                                 /* [1] Battery Strength */
    rep[1] = b->plug != 4;                               /* [2] bit 0: Charging */
}

void sinput_encode_features(u8 *rep, const struct gp_sinput_caps *caps, const u8 *serial6)
{
    memset(rep, 0, SINPUT_REPORT_LEN);
    rep[0] = 0x02;                                       /* command: features */
    put16(rep, 2, 1);                                    /* protocol version 1 */
    rep[3] = caps->features[0];
    rep[4] = caps->features[1];
    rep[5] = caps->gamepad_type;
    rep[6] = caps->face_style;
    put16(rep, 8, GP_TICK_MS * 1000);                    /* polling interval, us */
    put16(rep, 10, caps->accel_g);
    put16(rep, 12, caps->gyro_dps);
    memcpy(&rep[13], caps->button_mask, 4);
    memcpy(&rep[19], serial6, 6);                        /* [20..25] serial */
}
