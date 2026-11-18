/* test_sinput.c: the SInput descriptor and encoders (core/gp_sinput_desc.c),
 * against docs/sinput.md. */
#include "test.h"
#include "gp_sinput.h"

static u16 get16(const u8 *rep, int k)              /* k: byte index as in docs/sinput.md */
{
    return rep[k - 1] | rep[k] << 8;
}

/* ---- A small HID report descriptor parser: bits per report ID and
 * direction, and whether the collections close. */
struct hid_sizes {
    u32 in_bits[8], out_bits[8];
    int depth, ok;
};

static struct hid_sizes hid_parse(const u8 *d, int len)
{
    struct hid_sizes r = { .ok = 1 };
    u32 size = 0, count = 0;
    int id = 0;
    for (int i = 0; i < len;) {
        u8 b = d[i];
        int n = b & 3;
        n = n == 3 ? 4 : n;
        if (i + 1 + n > len) {
            r.ok = 0;                                    /* an item runs past the end */
            break;
        }
        u32 v = 0;
        for (int k = 0; k < n; k++) {
            v |= (u32)d[i + 1 + k] << (8 * k);
        }
        switch (b & 0xfc) {
        case 0x74: size = v; break;                      /* Report Size */
        case 0x94: count = v; break;                     /* Report Count */
        case 0x84: id = v; break;                        /* Report ID */
        case 0x80: if (id < 8) r.in_bits[id] += size * count; break;    /* Input */
        case 0x90: if (id < 8) r.out_bits[id] += size * count; break;   /* Output */
        case 0xa0: r.depth++; break;                     /* Collection */
        case 0xc0: r.depth--; break;                     /* End Collection */
        }
        i += 1 + n;
    }
    return r;
}

static void descriptor_report_sizes(void)
{
    CHECK_EQ(sizeof(sinput_report_desc), SINPUT_REPORT_DESC_LEN);
    struct hid_sizes r = hid_parse(sinput_report_desc, SINPUT_REPORT_DESC_LEN);
    CHECK(r.ok);
    CHECK_EQ(r.depth, 0);                                /* every collection closed */
    CHECK_EQ(r.in_bits[SINPUT_ID_STATE], SINPUT_REPORT_LEN * 8);
    CHECK_EQ(r.in_bits[SINPUT_ID_FEATURES], SINPUT_REPORT_LEN * 8);
    CHECK_EQ(r.in_bits[SINPUT_ID_BATTERY], SINPUT_REPORT_LEN * 8);
    CHECK_EQ(r.out_bits[SINPUT_ID_COMMAND], 47 * 8);
}

static void state_layout(void)
{
    struct gp_state s = {
        .buttons = 0x00c0ffee, .lx = -32767, .ly = 32767, .rx = 0x1234, .ry = -2,
        .lt = -32768, .rt = 32767, .imu = { 10, -20, 8192, -1, 2048, -32768 },
    };
    struct gp_battery b = { .percent = 77, .plug = 2, .charging = 1 };
    u8 rep[SINPUT_REPORT_LEN];
    memset(rep, 0xee, sizeof(rep));
    sinput_encode_state(rep, &s, &b, 0x89abcdef);
    CHECK_EQ(rep[0], 2);                                 /* [1] plug */
    CHECK_EQ(rep[1], 77);                                /* [2] percent */
    CHECK_EQ(rep[2], 0xee);                              /* [3..5] buttons */
    CHECK_EQ(rep[3], 0xff);
    CHECK_EQ(rep[4], 0xc0);
    CHECK_EQ(rep[5], 1);                                 /* [6] charging */
    CHECK_EQ((s16)get16(rep, 7), -32767);
    CHECK_EQ((s16)get16(rep, 9), 32767);
    CHECK_EQ(get16(rep, 11), 0x1234);
    CHECK_EQ((s16)get16(rep, 13), -2);
    CHECK_EQ((s16)get16(rep, 15), -32768);
    CHECK_EQ((s16)get16(rep, 17), 32767);
    CHECK_EQ(get16(rep, 19) | (u32)get16(rep, 21) << 16, 0x89abcdef);   /* timestamp, us */
    for (int i = 0; i < 6; i++) {
        CHECK_EQ((s16)get16(rep, 23 + 2 * i), s.imu[i]);
    }
    for (int k = 35; k < SINPUT_REPORT_LEN; k++) {
        CHECK_EQ(rep[k], 0);                             /* the rest is zero, not stale */
    }
}

static void battery_report(void)
{
    struct gp_battery b = { .percent = 55, .plug = 4 };
    u8 rep[SINPUT_REPORT_LEN];
    sinput_encode_battery(rep, &b);
    CHECK_EQ(rep[0], 55);
    CHECK_EQ(rep[1], 0);                                 /* on battery: not charging */
    b.plug = 3;                                          /* full, on 5 V: charging for Linux */
    sinput_encode_battery(rep, &b);
    CHECK_EQ(rep[1], 1);
}

static void battery_due(void)
{
    struct gp_battery b = { .percent = 80, .plug = 4 };
    u32 t = 5000;
    sinput_battery_reset();
    CHECK_EQ(sinput_battery_due(&b, t), 1);              /* a new host: at once */
    CHECK_EQ(sinput_battery_due(&b, t + 10), 0);
    CHECK_EQ(sinput_battery_due(&b, t + SINPUT_BATTERY_MS - 1), 0);
    CHECK_EQ(sinput_battery_due(&b, t + SINPUT_BATTERY_MS), 1);     /* once a minute */
    t += SINPUT_BATTERY_MS;
    b.percent = 79;
    CHECK_EQ(sinput_battery_due(&b, t + 1), 1);          /* the percent changed */
    CHECK_EQ(sinput_battery_due(&b, t + 2), 0);
    b.plug = 2;
    CHECK_EQ(sinput_battery_due(&b, t + 3), 1);          /* 5 V came */
    b.plug = 3;
    CHECK_EQ(sinput_battery_due(&b, t + 4), 0);          /* charging -> full: still on 5 V, same */
    sinput_battery_reset();
    CHECK_EQ(sinput_battery_due(&b, t + 5), 1);
}

static void features_layout(void)
{
    struct gp_sinput_caps c = {
        .features = { 0xff, 0x02 }, .gamepad_type = 3, .face_style = 0x21,
        .accel_g = 4, .gyro_dps = 2048, .button_mask = { 0xaa, 0xbb, 0xcc, 0xdd },
    };
    const u8 serial[6] = { 1, 2, 3, 4, 5, 6 };
    u8 rep[SINPUT_REPORT_LEN];
    sinput_encode_features(rep, &c, serial);
    CHECK_EQ(rep[0], 0x02);                              /* command: features */
    CHECK_EQ(get16(rep, 2), 1);                          /* protocol version */
    CHECK_EQ(rep[3], 0xff);
    CHECK_EQ(rep[4], 0x02);
    CHECK_EQ(rep[5], 3);
    CHECK_EQ(rep[6], 0x21);
    CHECK_EQ(get16(rep, 8), GP_TICK_MS * 1000);          /* polling interval, us */
    CHECK_EQ(get16(rep, 10), 4);
    CHECK_EQ(get16(rep, 12), 2048);
    CHECK(!memcmp(&rep[13], c.button_mask, 4));
    CHECK(!memcmp(&rep[19], serial, 6));
}

int main(void)
{
    printf("test_sinput\n");
    RUN(descriptor_report_sizes);
    RUN(state_layout);
    RUN(battery_report);
    RUN(battery_due);
    RUN(features_layout);
    return t_done("test_sinput");
}
