/* test_ds4.c: DS4 mode's descriptors, reports and output parsing
 * (core/gp_ds4_desc.c), against the layouts Linux's hid-playstation reads:
 * report sizes it checks, the CRC-32 it checks over Bluetooth, the
 * calibration it scales the IMU by. */
#include <string.h>
#include "test.h"
#include "gp_ds4.h"

static u16 get16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

static u32 get32(const u8 *p)
{
    return get16(p) | (u32)get16(p + 2) << 16;
}

/* ---- Report sizes per ID and kind, and whether the collections close. */
struct hid_sizes {
    u32 in_bits[256], out_bits[256], feat_bits[256];
    int depth, ok;
};

static struct hid_sizes r;

static void hid_parse(const u8 *d, int len)
{
    memset(&r, 0, sizeof(r));
    r.ok = 1;
    u32 size = 0, count = 0;
    int id = 0;
    for (int i = 0; i < len;) {
        u8 b = d[i];
        int n = b & 3;
        n = n == 3 ? 4 : n;
        if (i + 1 + n > len) {
            r.ok = 0;
            break;
        }
        u32 v = 0;
        for (int k = 0; k < n; k++) {
            v |= (u32)d[i + 1 + k] << (8 * k);
        }
        switch (b & 0xfc) {
        case 0x74: size = v; break;
        case 0x94: count = v; break;
        case 0x84: id = v & 0xff; break;
        case 0x80: r.in_bits[id] += size * count; break;
        case 0x90: r.out_bits[id] += size * count; break;
        case 0xb0: r.feat_bits[id] += size * count; break;
        case 0xa0: r.depth++; break;
        case 0xc0: r.depth--; break;
        }
        i += 1 + n;
    }
}

static void descriptors(void)
{
    hid_parse(ds4_usb_report_desc, ds4_usb_report_desc_len);
    CHECK(r.ok);
    CHECK_EQ(r.depth, 0);
    CHECK_EQ(r.in_bits[DS4_ID_USB_STATE], DS4_USB_REPORT_LEN * 8);   /* 64 B with the ID */
    CHECK_EQ(r.out_bits[DS4_ID_USB_OUTPUT], 31 * 8);
    CHECK_EQ(r.feat_bits[DS4_FEAT_CAL_USB], 36 * 8);
    CHECK_EQ(r.feat_bits[DS4_FEAT_PAIRING], 15 * 8);
    CHECK_EQ(r.feat_bits[DS4_FEAT_FIRMWARE], 48 * 8);

    hid_parse(ds4_bt_report_desc, ds4_bt_report_desc_len);
    CHECK(r.ok);
    CHECK_EQ(r.depth, 0);
    CHECK_EQ(r.in_bits[DS4_ID_BT_STATE], DS4_BT_REPORT_LEN * 8);     /* 78 B with the ID */
    CHECK_EQ(r.out_bits[DS4_ID_BT_OUTPUT], 77 * 8);
    CHECK_EQ(r.feat_bits[DS4_FEAT_CAL_BT], 40 * 8);
    CHECK_EQ(r.feat_bits[DS4_FEAT_FIRMWARE], 48 * 8);
    CHECK(ds4_bt_report_desc_len <= 334);                /* the SDP record has room for Xbox's 334 B */
}

static void crc32(void)
{
    /* The standard check value: CRC-32 of "123456789". */
    CHECK_EQ(ds4_crc32('1', (const u8 *)"23456789", 8), 0xCBF43926u);
    CHECK_EQ(ds4_crc32(0, NULL, 0), 0xD202EF8Du);        /* one zero byte */
}

static const struct gp_battery on_battery = { .percent = 55, .plug = 4 };

static void encode(u8 *rep, const struct gp_state *s)
{
    ds4_encode_usb(rep, s, &on_battery, 0, 0);
}

static void sticks_and_triggers(void)
{
    struct gp_state s = { .lt = -32768, .rt = -32768 };
    u8 rep[DS4_USB_REPORT_LEN];
    encode(rep, &s);
    CHECK_EQ(rep[0], 128);                               /* centred */
    CHECK_EQ(rep[3], 128);
    CHECK_EQ(rep[7], 0);                                 /* released */
    CHECK_EQ(rep[8], 0);
    CHECK_EQ(rep[5] & 0x0c, 0);                          /* no L2 / R2 bits */
    s.lx = -32767;
    s.ly = 32767;                                        /* +Y down, as the DS4 */
    s.rx = 32767;
    s.ry = -32767;
    s.lt = 32767;
    s.rt = 0;
    encode(rep, &s);
    CHECK_EQ(rep[0], 0);
    CHECK_EQ(rep[1], 255);
    CHECK_EQ(rep[2], 255);
    CHECK_EQ(rep[3], 0);
    CHECK_EQ(rep[7], 255);
    CHECK_EQ(rep[8], 128);
    CHECK_EQ(rep[5] & 0x0c, 0x0c);                       /* off rest: the bits too */
    /* a button remapped to L2 / R2 pulls the trigger fully */
    struct gp_state m = { .lt = -32768, .rt = -32768, .buttons = GP_BIT(GP_BTN_L2) | GP_BIT(GP_BTN_R2) };
    encode(rep, &m);
    CHECK_EQ(rep[7], 255);
    CHECK_EQ(rep[8], 255);
    CHECK_EQ(rep[5] & 0x0c, 0x0c);
}

static u32 bits_of(u32 buttons)
{
    struct gp_state s = { .lt = -32768, .rt = -32768, .buttons = buttons };
    u8 rep[DS4_USB_REPORT_LEN];
    encode(rep, &s);
    return (rep[4] & 0xf0) | rep[5] << 8 | (rep[6] & 0x03) << 16;
}

static void buttons(void)
{
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_WEST)), 0x10);        /* Square */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_SOUTH)), 0x20);       /* Cross */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_EAST)), 0x40);        /* Circle */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_NORTH)), 0x80);       /* Triangle */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_L1)), 0x0100);
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_R1)), 0x0200);
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_BACK)), 0x1000);      /* Share */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_START)), 0x2000);     /* Options */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_L3)), 0x4000);
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_R3)), 0x8000);
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_GUIDE)), 0x10000);    /* PS */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_CAPTURE)), 0x20000);  /* touchpad click */
    /* no DS4 button: dropped */
    CHECK_EQ(bits_of(GP_BIT(GP_BTN_PADDLE_L1) | GP_BIT(GP_BTN_PADDLE_R1) | GP_BIT(GP_BTN_MODIFIER)), 0);
}

static u8 hat_of(u32 buttons)
{
    struct gp_state s = { .lt = -32768, .rt = -32768, .buttons = buttons };
    u8 rep[DS4_USB_REPORT_LEN];
    encode(rep, &s);
    return rep[4] & 0x0f;
}

static void hat(void)
{
    const u32 U = GP_BIT(GP_BTN_UP), D = GP_BIT(GP_BTN_DOWN), L = GP_BIT(GP_BTN_LEFT), R = GP_BIT(GP_BTN_RIGHT);
    CHECK_EQ(hat_of(0), 8);
    CHECK_EQ(hat_of(U), 0);
    CHECK_EQ(hat_of(U | R), 1);
    CHECK_EQ(hat_of(R), 2);
    CHECK_EQ(hat_of(D | R), 3);
    CHECK_EQ(hat_of(D), 4);
    CHECK_EQ(hat_of(D | L), 5);
    CHECK_EQ(hat_of(L), 6);
    CHECK_EQ(hat_of(U | L), 7);
    CHECK_EQ(hat_of(U | D), 8);
    CHECK_EQ(hat_of(L | R), 8);
}

static void imu_counter_timestamp(void)
{
    struct gp_state s = { .lt = -32768, .rt = -32768, .imu = { 1, -2, 8192, 16, -32768, 32767 } };
    u8 rep[DS4_USB_REPORT_LEN];
    ds4_encode_usb(rep, &s, &on_battery, 16000, 0x45);
    /* the DS4's frame: (-X, Z, -Y) of SInput's, negation saturating */
    CHECK_EQ((s16)get16(&rep[12]), -16);                 /* gyro first */
    CHECK_EQ((s16)get16(&rep[14]), 32767);
    CHECK_EQ((s16)get16(&rep[16]), 32767);
    CHECK_EQ((s16)get16(&rep[18]), -1);                  /* then accel */
    CHECK_EQ((s16)get16(&rep[20]), 8192);                /* flat: +1 g on Y, as a real DS4 */
    CHECK_EQ((s16)get16(&rep[22]), 2);
    CHECK_EQ(rep[6] >> 2, 0x05);                         /* counter, 6 bits */
    CHECK_EQ(get16(&rep[9]), 3000);                      /* 16 ms in 16/3 µs units */
    ds4_encode_usb(rep, &s, &on_battery, 0xffffffffu, 0);   /* no overflow at the end of u32 */
    CHECK_EQ(get16(&rep[9]), (u16)(0xffffffffull * 3 / 16));
}

static u8 battery_of(u8 percent, u8 plug)
{
    struct gp_state s = { .lt = -32768, .rt = -32768 };
    struct gp_battery b = { .percent = percent, .plug = plug };
    u8 rep[DS4_USB_REPORT_LEN];
    ds4_encode_usb(rep, &s, &b, 0, 0);
    return rep[29];
}

static void battery(void)
{
    CHECK_EQ(battery_of(55, 4), 5);                      /* on battery: tenths */
    CHECK_EQ(battery_of(100, 4), 10);
    CHECK_EQ(battery_of(3, 4), 0);
    CHECK_EQ(battery_of(55, 2), 0x15);                   /* charging: the cable bit */
    CHECK_EQ(battery_of(100, 2), 0x1a);
    CHECK_EQ(battery_of(100, 3), 0x1b);                  /* full: 11 */
}

static void touch_none(void)
{
    struct gp_state s = { .lt = -32768, .rt = -32768 };
    u8 rep[DS4_USB_REPORT_LEN];
    encode(rep, &s);
    CHECK_EQ(rep[32], 1);                                /* one touch report */
    CHECK_EQ(rep[34], 0x80);                             /* contact 1 up */
    CHECK_EQ(rep[38], 0x80);                             /* contact 2 up */
}

static void bluetooth_report(void)
{
    struct gp_state s = { .lx = 1000, .lt = 0, .rt = -32768, .buttons = GP_BIT(GP_BTN_SOUTH),
                          .imu = { 5, 6, 7, 8, 9, 10 } };
    u8 usb[DS4_USB_REPORT_LEN], bt[DS4_BT_REPORT_LEN];
    ds4_encode_usb(usb, &s, &on_battery, 1234567, 9);
    ds4_encode_bt(bt, &s, &on_battery, 1234567, 9);
    CHECK_EQ(bt[0], 0xc0);
    CHECK_EQ(bt[1], 0x00);
    CHECK(memcmp(bt + 2, usb, 32 + 10) == 0);            /* the common part and the touch report */
    /* hid-playstation: CRC-32 over A1, the ID and the report but its last 4 bytes */
    u8 full[1 + DS4_BT_REPORT_LEN];
    full[0] = DS4_ID_BT_STATE;
    memcpy(full + 1, bt, DS4_BT_REPORT_LEN);
    CHECK_EQ(get32(&bt[DS4_BT_REPORT_LEN - 4]), ds4_crc32(DS4_CRC_INPUT, full, 1 + DS4_BT_REPORT_LEN - 4));
    bt[10] ^= 1;
    memcpy(full + 1, bt, DS4_BT_REPORT_LEN);
    CHECK(get32(&bt[DS4_BT_REPORT_LEN - 4]) != ds4_crc32(DS4_CRC_INPUT, full, 1 + DS4_BT_REPORT_LEN - 4));
}

/* hid-playstation's scaling (dualshock4_get_calibration_data): the raw
 * value times this is in its units (1024 per °/s, 8192 per g). */
static void calibration_scale(const u8 *c, int bt)
{
    int pitch_plus = (s16)get16(&c[7]);
    int pitch_minus = (s16)get16(bt ? &c[13] : &c[9]);
    int yaw_plus = (s16)get16(bt ? &c[9] : &c[11]);
    int yaw_minus = (s16)get16(bt ? &c[15] : &c[13]);
    int speed_2x = (s16)get16(&c[19]) + (s16)get16(&c[21]);
    CHECK_EQ((s16)get16(&c[1]), 0);                      /* no bias */
    CHECK_EQ(speed_2x * 1024 / (pitch_plus - pitch_minus), 64);   /* 16 LSB per °/s */
    CHECK_EQ(speed_2x * 1024 / (yaw_plus - yaw_minus), 64);
    int x_range = (s16)get16(&c[23]) - (s16)get16(&c[25]);
    int z_range = (s16)get16(&c[31]) - (s16)get16(&c[33]);
    CHECK_EQ(2 * 8192 / x_range, 1);                     /* 8192 per g */
    CHECK_EQ(2 * 8192 / z_range, 1);
    CHECK_EQ((s16)get16(&c[23]) - x_range / 2, 0);       /* accel bias */
}

static void features(void)
{
    static const u8 mac[6] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    u8 f[DS4_FEAT_MAX_LEN];
    /* sizes hid-playstation asks for (it fails the probe on another) */
    CHECK_EQ(ds4_feature(DS4_FEAT_CAL_USB, 0, mac, f), 37);
    CHECK_EQ(f[0], DS4_FEAT_CAL_USB);
    calibration_scale(f, 0);
    CHECK_EQ(ds4_feature(DS4_FEAT_CAL_BT, 1, mac, f), 41);
    CHECK_EQ(f[0], DS4_FEAT_CAL_BT);
    calibration_scale(f, 1);
    CHECK_EQ(get32(&f[37]), ds4_crc32(DS4_CRC_FEATURE, f, 37));
    CHECK_EQ(ds4_feature(DS4_FEAT_PAIRING, 0, mac, f), 16);
    CHECK_EQ(f[0], DS4_FEAT_PAIRING);
    CHECK_EQ(f[1], 0xff);                                /* LSB first */
    CHECK_EQ(f[6], 0xaa);
    CHECK_EQ(ds4_feature(DS4_FEAT_FIRMWARE, 1, mac, f), 49);
    CHECK_EQ(f[0], DS4_FEAT_FIRMWARE);
    CHECK_EQ(get16(&f[35]), DS4_VERSION);
    CHECK_EQ(get16(&f[41]), DS4_VERSION);
    CHECK_EQ(ds4_feature(0xf0, 0, mac, f), 0);           /* the PS4's auth: not ours */
    CHECK_EQ(ds4_feature(0x81, 1, mac, f), 0);
}

static void output(void)
{
    struct ds4_out o;
    /* USB 0x05 as hid-playstation / SDL send it: flags 07, then weak, strong, R G B */
    const u8 usb[32] = { 0x05, 0x07, 0x00, 0x00, 0x40, 0xc0, 1, 2, 3 };
    CHECK(ds4_parse_output(usb, sizeof(usb), &o));
    CHECK_EQ(o.valid, DS4_OUT_RUMBLE | DS4_OUT_LIGHTBAR);
    CHECK_EQ(o.small, 0x40);
    CHECK_EQ(o.big, 0xc0);
    CHECK_EQ(o.rgb[0], 1);
    CHECK_EQ(o.rgb[2], 3);
    /* Bluetooth 0x11: C0 | rate, audio, then the same */
    const u8 bt[78] = { 0x11, 0xc4, 0x00, 0x01, 0x00, 0x00, 0x10, 0x20, 9, 9, 9 };
    CHECK(ds4_parse_output(bt, sizeof(bt), &o));
    CHECK_EQ(o.valid, DS4_OUT_RUMBLE);                   /* lightbar not flagged: left alone */
    CHECK_EQ(o.small, 0x10);
    CHECK_EQ(o.big, 0x20);
    const u8 lb[78] = { 0x11, 0xc0, 0x00, 0x02, 0x00, 0x00, 0, 0, 0, 0, 64 };
    CHECK(ds4_parse_output(lb, sizeof(lb), &o));
    CHECK_EQ(o.valid, DS4_OUT_LIGHTBAR);
    CHECK_EQ(o.rgb[2], 64);
    CHECK(!ds4_parse_output(usb, 5, &o));                /* too short */
    const u8 other[32] = { 0x03, 0x07 };
    CHECK(!ds4_parse_output(other, sizeof(other), &o));
    CHECK(!ds4_parse_output(usb, 0, &o));
}

int main(void)
{
    printf("test_ds4\n");
    RUN(descriptors);
    RUN(crc32);
    RUN(sticks_and_triggers);
    RUN(buttons);
    RUN(hat);
    RUN(imu_counter_timestamp);
    RUN(battery);
    RUN(touch_none);
    RUN(bluetooth_report);
    RUN(features);
    RUN(output);
    return t_done("test_ds4");
}
