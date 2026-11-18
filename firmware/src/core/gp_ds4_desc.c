/* gp_ds4_desc.c: DS4 mode's descriptors, reports and output parsing (see
 * gp_ds4.h), hardware-free, so the host tests build them. Layouts as
 * Linux's hid-playstation (struct dualshock4_input_report_*) and SDL's
 * SDL_hidapi_ps4.c read them. */
#include "system/includes.h"
#include "gp_ds4.h"

/* USB: report 1 with gamepad usages, so a generic HID driver (Windows'
 * DirectInput, a browser) sees sticks, hat, 14 buttons and the triggers as
 * a real DS4 shows them; the rest of it vendor bytes (IMU, battery,
 * touchpad). Output 5; features 2, 0x12, 0xA3. */
const u8 ds4_usb_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,                 /* Generic Desktop / Gamepad / Application */
    0x85, DS4_ID_USB_STATE,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,     /* [1..4] LX LY RX RY: X Y Z Rz, as the DS4 */
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00,     /* [5] bits 0..3: hat, 8 = released */
    0x46, 0x3b, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x65, 0x00, 0x45, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00,     /* [5..7] 14 buttons: Square Cross Circle Triangle, */
    0x25, 0x01, 0x75, 0x01, 0x95, 0x0e, 0x81, 0x02,     /* L1 R1 L2 R2 Share Options L3 R3, PS, touchpad */
    0x06, 0x00, 0xff, 0x09, 0x20, 0x25, 0x3f,           /* [7] bits 2..7: report counter */
    0x75, 0x06, 0x95, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x26, 0xff, 0x00,   /* [8, 9] L2 R2: Rx Ry */
    0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0x06, 0x00, 0xff, 0x09, 0x21, 0x95, 0x36, 0x81, 0x02,   /* [10..63] IMU, battery, touchpad */
    0x85, DS4_ID_USB_OUTPUT, 0x09, 0x22, 0x95, 0x1f, 0x91, 0x02,   /* output 5: 31 B */
    0x85, DS4_FEAT_CAL_USB, 0x09, 0x24, 0x95, 0x24, 0xb1, 0x02,    /* feature 2: 36 B */
    0x85, DS4_FEAT_PAIRING, 0x09, 0x25, 0x95, 0x0f, 0xb1, 0x02,    /* feature 0x12: 15 B */
    0x85, DS4_FEAT_FIRMWARE, 0x09, 0x26, 0x95, 0x30, 0xb1, 0x02,   /* feature 0xA3: 48 B */
    0xc0,
};
const u16 ds4_usb_report_desc_len = sizeof(ds4_usb_report_desc);

/* Bluetooth: the full report 0x11 both ways (vendor bytes: hid-playstation
 * and SDL parse it raw), features 5 and 0xA3. */
const u8 ds4_bt_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01,
    0x06, 0x00, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
    0x85, DS4_ID_BT_STATE, 0x09, 0x20, 0x95, DS4_BT_REPORT_LEN, 0x81, 0x02,   /* input 0x11: 77 B */
    0x85, DS4_ID_BT_OUTPUT, 0x09, 0x21, 0x95, 0x4d, 0x91, 0x02,               /* output 0x11: 77 B */
    0x85, DS4_FEAT_CAL_BT, 0x09, 0x22, 0x95, 0x28, 0xb1, 0x02,                /* feature 5: 40 B */
    0x85, DS4_FEAT_FIRMWARE, 0x09, 0x23, 0x95, 0x30, 0xb1, 0x02,              /* feature 0xA3: 48 B */
    0xc0,
};
const u16 ds4_bt_report_desc_len = sizeof(ds4_bt_report_desc);

/* ---- CRC-32 ----------------------------------------------------------------
 * Reflected, polynomial 0xEDB88320, 4 bits a step (a 16-entry table: 78
 * bytes take ~160 steps, a few µs at 96 MHz). */
static const u32 crc_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

static u32 crc_byte(u32 c, u8 b)
{
    c ^= b;
    c = (c >> 4) ^ crc_nibble[c & 15];
    c = (c >> 4) ^ crc_nibble[c & 15];
    return c;
}

u32 ds4_crc32(u8 seed, const u8 *data, int len)
{
    u32 c = crc_byte(0xffffffff, seed);
    for (int i = 0; i < len; i++) {
        c = crc_byte(c, data[i]);
    }
    return ~c;
}

static void put16(u8 *p, int v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

/* ---- The state ---------------------------------------------------------------
 * The part both reports share (hid-playstation's
 * dualshock4_input_report_common, 32 B):
 *   0..3 LX LY RX RY (u8, centre 128, +Y down)   4 hat | Square Cross Circle Triangle << 4
 *   5 L1 R1 L2 R2 Share Options L3 R3            6 PS, touchpad click, counter << 2
 *   7, 8 L2 R2 (u8)                              9, 10 timestamp (5.33 µs units)
 *   11 temperature                               12..17 gyro X Y Z, 18..23 accel X Y Z (s16)
 *   24..28 reserved                              29 battery 0..10 | cable << 4 (11: full)
 *   30, 31 reserved
 * Then one touch report: none, both contacts up (bit 7). */
#define DS4_COMMON_LEN      32

static u8 stick8(s16 v)
{
    return (u16)(v + 32768) >> 8;
}

/* Hat: 0 N, 1 NE .. 7 NW clockwise, 8 released. */
static u8 ds4_hat(u32 b)
{
    static const u8 hat[16] = {
        /* bits: up 1, down 2, left 4, right 8 */
        8, 0, 4, 8, 6, 7, 5, 8, 2, 1, 3, 8, 8, 8, 8, 8,
    };
    u8 i = ((b & GP_BIT(GP_BTN_UP)) ? 1 : 0) | ((b & GP_BIT(GP_BTN_DOWN)) ? 2 : 0) |
           ((b & GP_BIT(GP_BTN_LEFT)) ? 4 : 0) | ((b & GP_BIT(GP_BTN_RIGHT)) ? 8 : 0);
    return hat[i];
}

/* gp_state button -> bit in bytes 4..6 of the common part (byte * 8 + bit).
 * Back is Share, Start Options, Guide PS, Capture the touchpad click (as
 * on pads that have no touchpad). The paddles have no DS4 button: dead
 * unless remapped. */
static const u8 ds4_map[][2] = {
    { GP_BTN_WEST, 4 }, { GP_BTN_SOUTH, 5 }, { GP_BTN_EAST, 6 }, { GP_BTN_NORTH, 7 },
    { GP_BTN_L1, 8 }, { GP_BTN_R1, 9 }, { GP_BTN_BACK, 12 }, { GP_BTN_START, 13 },
    { GP_BTN_L3, 14 }, { GP_BTN_R3, 15 }, { GP_BTN_GUIDE, 16 }, { GP_BTN_CAPTURE, 17 },
};

/* The battery nibble: 0..10 tenths on battery; on 5 V 0..10 charging, 11 full. */
static u8 ds4_battery(const struct gp_battery *b)
{
    u8 level = b->percent >= 100 ? 10 : b->percent / 10;
    if (b->plug == 4) {
        return level;
    }
    return 0x10 | (b->plug == 3 ? 11 : level);
}

/* The IMU in the DS4's frame. Units as SInput's (8192 LSB per g, 16 LSB
 * per °/s, docs/sinput.md 2.2; the calibration report says so,
 * ds4_feature), axes not: SDL reads the DS4's as they are (SDL_hidapi_ps4.c)
 * and SInput's as (-X, Z, -Y) (SDL_hidapi_sinput.c), so DS4 = (-X, Z, -Y)
 * of SInput. Flat on a desk: DS4 accel Y +1 g, as a real one (measured
 * 2026-11-17: Z +7878 before this). Negation saturates. */
static s16 ds4_neg(s16 v)
{
    return v == -32768 ? 32767 : -v;
}

static void ds4_imu(u8 *c, const s16 *v)
{
    put16(&c[0], ds4_neg(v[0]));
    put16(&c[2], v[2]);
    put16(&c[4], ds4_neg(v[1]));
}

static void ds4_common(u8 *c, const struct gp_state *s, const struct gp_battery *b, u32 ts_us, u8 counter)
{
    memset(c, 0, DS4_COMMON_LEN);
    c[0] = stick8(s->lx);
    c[1] = stick8(s->ly);
    c[2] = stick8(s->rx);
    c[3] = stick8(s->ry);
    u32 bits = 0;
    for (int i = 0; i < ARRAY_SIZE(ds4_map); i++) {
        if (s->buttons & GP_BIT(ds4_map[i][0])) {
            bits |= 1u << ds4_map[i][1];
        }
    }
    /* Triggers: analog, the button bit once off rest. A button remapped
     * to L2 / R2 (gp_settings) pulls its trigger fully too. */
    u8 lt = stick8(s->lt), rt = stick8(s->rt);
    if (s->buttons & GP_BIT(GP_BTN_L2)) {
        lt = 255;
    }
    if (s->buttons & GP_BIT(GP_BTN_R2)) {
        rt = 255;
    }
    if (lt) {
        bits |= 1u << 10;
    }
    if (rt) {
        bits |= 1u << 11;
    }
    c[4] = (u8)bits | ds4_hat(s->buttons);
    c[5] = bits >> 8;
    c[6] = (bits >> 16) | (counter & 0x3f) << 2;
    c[7] = lt;
    c[8] = rt;
    /* floor(ts_us * 3 / 16) without overflowing: 16 / 3 µs a unit */
    put16(&c[9], (ts_us / 16) * 3 + (ts_us % 16) * 3 / 16);
    ds4_imu(&c[12], &s->imu[3]);                        /* gyro first */
    ds4_imu(&c[18], &s->imu[0]);
    c[29] = ds4_battery(b);
}

/* One touch report: timestamp, two contacts, both up. */
static void ds4_no_touch(u8 *t)
{
    t[0] = 1;                                           /* one touch report */
    t[1] = 0;
    t[2] = 0x80;
    t[6] = 0x80;
}

void ds4_encode_usb(u8 *rep, const struct gp_state *s, const struct gp_battery *b, u32 ts_us, u8 counter)
{
    memset(rep, 0, DS4_USB_REPORT_LEN);
    ds4_common(rep, s, b, ts_us, counter);
    ds4_no_touch(rep + DS4_COMMON_LEN);
}

/* Bluetooth 0x11: 2 bytes first (0xC0: "HID + CRC", as a DS4 sends it),
 * the common part, touch, then CRC-32 over A1 11 and the 73 bytes before it. */
void ds4_encode_bt(u8 *rep, const struct gp_state *s, const struct gp_battery *b, u32 ts_us, u8 counter)
{
    memset(rep, 0, DS4_BT_REPORT_LEN);
    rep[0] = 0xc0;
    ds4_common(rep + 2, s, b, ts_us, counter);
    ds4_no_touch(rep + 2 + DS4_COMMON_LEN);
    u8 id = DS4_ID_BT_STATE;
    u32 c = crc_byte(crc_byte(0xffffffff, DS4_CRC_INPUT), id);
    for (int i = 0; i < DS4_BT_REPORT_LEN - 4; i++) {
        c = crc_byte(c, rep[i]);
    }
    put32(&rep[DS4_BT_REPORT_LEN - 4], ~c);
}

/* ---- Feature reports -----------------------------------------------------------
 * Calibration (0x02 USB, 0x05 Bluetooth), s16: gyro bias pitch yaw roll,
 * then the six gyro references (USB: pitch +, -, yaw +, -, roll +, -;
 * Bluetooth: the three + then the three -), the gyro speed they stand for
 * (+, -), accel X + -, Y + -, Z + -. hid-playstation and SDL scale by
 * speed * 2 / (plus - minus) and 2 g / (plus - minus): with 8640 / -8640
 * at 540 °/s and 8192 / -8192 that is 16 LSB per °/s and 8192 per g, the
 * report's own units, and no bias (gp_motion.c removes the gyro's). */
#define DS4_GYRO_REF        8640            /* 540 °/s x 16 */
#define DS4_GYRO_SPEED      540
#define DS4_ACCEL_REF       8192            /* 1 g */
#define DS4_CAL_USB_LEN     37
#define DS4_CAL_BT_LEN      41
#define DS4_PAIRING_LEN     16
#define DS4_FIRMWARE_LEN    49

static int ds4_calibration(int bt, u8 *out)
{
    memset(out, 0, DS4_FEAT_MAX_LEN);
    out[0] = bt ? DS4_FEAT_CAL_BT : DS4_FEAT_CAL_USB;
    for (int k = 0; k < 3; k++) {
        if (bt) {
            put16(&out[7 + 2 * k], DS4_GYRO_REF);
            put16(&out[13 + 2 * k], -DS4_GYRO_REF);
        } else {
            put16(&out[7 + 4 * k], DS4_GYRO_REF);
            put16(&out[9 + 4 * k], -DS4_GYRO_REF);
        }
        put16(&out[23 + 4 * k], DS4_ACCEL_REF);
        put16(&out[25 + 4 * k], -DS4_ACCEL_REF);
    }
    put16(&out[19], DS4_GYRO_SPEED);
    put16(&out[21], DS4_GYRO_SPEED);
    if (!bt) {
        return DS4_CAL_USB_LEN;
    }
    put32(&out[DS4_CAL_BT_LEN - 4], ds4_crc32(DS4_CRC_FEATURE, out, DS4_CAL_BT_LEN - 4));
    return DS4_CAL_BT_LEN;
}

int ds4_feature(u8 id, int bt, const u8 *mac, u8 *out)
{
    switch (id) {
    case DS4_FEAT_CAL_USB:
    case DS4_FEAT_CAL_BT:
        return ds4_calibration(id == DS4_FEAT_CAL_BT, out);
    case DS4_FEAT_PAIRING:                              /* the pad's address, LSB first; no host paired */
        memset(out, 0, DS4_FEAT_MAX_LEN);
        out[0] = id;
        for (int k = 0; k < 6; k++) {
            out[1 + k] = mac[5 - k];
        }
        out[7] = 0x08;                                  /* 08 25 00, as a DS4 */
        out[8] = 0x25;
        return DS4_PAIRING_LEN;
    case DS4_FEAT_FIRMWARE: {                           /* build date and time, then the versions */
        static const char date[] = "Nov 17 2026", time[] = "00:00:00";
        memset(out, 0, DS4_FEAT_MAX_LEN);
        out[0] = id;
        memcpy(&out[1], date, sizeof(date) - 1);
        memcpy(&out[17], time, sizeof(time) - 1);
        put16(&out[35], DS4_VERSION);                   /* hardware version (hid-playstation reads it here) */
        put16(&out[41], DS4_VERSION);                   /* firmware version */
        return DS4_FIRMWARE_LEN;
    }
    default:
        return 0;
    }
}

/* ---- Output ----------------------------------------------------------------------
 * USB 0x05: flags (bit 0 rumble, 1 lightbar, 2 blink), flags 2, 0, weak
 * (right), strong (left), R G B, blink on / off. Bluetooth 0x11: two bytes
 * (HID + CRC, poll rate; audio) and then the same. The CRC isn't checked: a
 * bad one could only come from the host's own stack. */
int ds4_parse_output(const u8 *c, int len, struct ds4_out *o)
{
    int at;
    if (len >= 1 && c[0] == DS4_ID_USB_OUTPUT) {
        at = 1;
    } else if (len >= 1 && c[0] == DS4_ID_BT_OUTPUT) {
        at = 3;
    } else {
        return 0;
    }
    memset(o, 0, sizeof(*o));
    if (len < at + 8) {
        return 0;
    }
    const u8 *p = c + at;
    if (p[0] & 0x01) {
        o->valid |= DS4_OUT_RUMBLE;
        o->small = p[3];
        o->big = p[4];
    }
    if (p[0] & 0x02) {
        o->valid |= DS4_OUT_LIGHTBAR;
        memcpy(o->rgb, p + 5, 3);
    }
    return 1;
}
