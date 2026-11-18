/* test_link.c: the pad <-> receiver link codec (core/gp_link.c). */
#include "test.h"
#include "gp_link.h"

/* An encoder writes exactly len bytes: the byte after stays as it was. */
#define GUARD 0xA5

static void state_round_trip(void)
{
    struct gp_state s = {
        .buttons = 0x00ffffff, .lx = -32768, .ly = 32767, .rx = -1, .ry = 0,
        .lt = -32768, .rt = 32767, .imu = { 1, -2, 300, -32768, 32767, -12345 },
    };
    u8 p[GP_LINK_STATE_LEN + 1];
    memset(p, GUARD, sizeof(p));
    gp_link_encode_state(p, 0xfe, &s, 0xbeef);
    CHECK_EQ(p[GP_LINK_STATE_LEN], GUARD);

    struct gp_state d;
    u8 seq;
    u16 ms;
    memset(&d, 0, sizeof(d));
    CHECK_EQ(gp_link_decode_state(p, GP_LINK_STATE_LEN, &seq, &d, &ms), 1);
    CHECK_EQ(seq, 0xfe);
    CHECK_EQ(ms, 0xbeef);
    CHECK_EQ(d.buttons, 0x00ffffff);
    CHECK_EQ(d.lx, -32768);
    CHECK_EQ(d.ly, 32767);
    CHECK_EQ(d.rx, -1);
    CHECK_EQ(d.ry, 0);
    CHECK_EQ(d.lt, -32768);
    CHECK_EQ(d.rt, 32767);
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(d.imu[i], s.imu[i]);
    }
}

static void state_buttons_above_24_dropped(void)
{
    /* Only the 24 reported bits travel (the modifier, bit 31, never does). */
    struct gp_state s = { .buttons = 0x80000001 };
    u8 p[GP_LINK_STATE_LEN];
    gp_link_encode_state(p, 0, &s, 0);
    struct gp_state d;
    u8 seq;
    u16 ms;
    gp_link_decode_state(p, sizeof(p), &seq, &d, &ms);
    CHECK_EQ(d.buttons, 1);
}

static void state_layout(void)
{
    /* docs/dongle.md: [0] seq [1..3] buttons [4..11] sticks [12..15] triggers
     * [16..27] IMU [28..29] ms, little-endian. */
    struct gp_state s = { .buttons = 0x030201, .lx = 0x0504, .lt = 0x0d0c, .imu = { 0x1110 } };
    u8 p[GP_LINK_STATE_LEN];
    gp_link_encode_state(p, 7, &s, 0x1d1c);
    CHECK_EQ(p[0], 7);
    CHECK_EQ(p[1], 0x01);
    CHECK_EQ(p[3], 0x03);
    CHECK_EQ(p[4], 0x04);
    CHECK_EQ(p[5], 0x05);
    CHECK_EQ(p[12], 0x0c);
    CHECK_EQ(p[16], 0x10);
    CHECK_EQ(p[28], 0x1c);
    CHECK_EQ(p[29], 0x1d);
}

static void short_packets_refused(void)
{
    u8 p[64] = { 0 };
    struct gp_state s;
    struct gp_battery b;
    struct gp_link_hello h;
    u8 seq;
    u16 ms;
    CHECK_EQ(gp_link_decode_state(p, GP_LINK_STATE_LEN - 1, &seq, &s, &ms), 0);
    CHECK_EQ(gp_link_decode_status(p, GP_LINK_STATUS_LEN - 1, &b), 0);
    CHECK_EQ(gp_link_decode_hello(p, GP_LINK_HELLO_LEN - 1, &h), 0);
    CHECK_EQ(gp_link_decode_status(p, GP_LINK_STATUS_LEN, &b), 1);
}

static void status_round_trip(void)
{
    struct gp_battery b = { .mv = 4187, .percent = 94, .plug = 4, .charging = 0 }, d;
    u8 p[GP_LINK_STATUS_LEN + 1];
    memset(p, GUARD, sizeof(p));
    gp_link_encode_status(p, &b);
    CHECK_EQ(p[GP_LINK_STATUS_LEN], GUARD);
    memset(&d, 0, sizeof(d));
    CHECK_EQ(gp_link_decode_status(p, GP_LINK_STATUS_LEN, &d), 1);
    CHECK_EQ(d.mv, 4187);
    CHECK_EQ(d.percent, 94);
    CHECK_EQ(d.plug, 4);
    CHECK_EQ(d.charging, 0);
}

static void hello_round_trip(void)
{
    struct gp_link_hello h = {
        .version = GP_LINK_VERSION, .mode = GP_MODE_XBOX,
        .caps = { .features = { 0xff, 0x02 }, .gamepad_type = 9, .face_style = 0x13,
                  .accel_g = 4, .gyro_dps = 2048, .button_mask = { 1, 2, 3, 4 } },
        .serial = { 0x60, 0x3f, 0x53, 0x3a, 0xdf, 0x60 }, .tick_us = 3000,
    }, d;
    u8 p[GP_LINK_HELLO_LEN + 1];
    memset(p, GUARD, sizeof(p));
    gp_link_encode_hello(p, &h);
    CHECK_EQ(p[GP_LINK_HELLO_LEN], GUARD);
    memset(&d, 0, sizeof(d));
    CHECK_EQ(gp_link_decode_hello(p, GP_LINK_HELLO_LEN, &d), 1);
    CHECK_EQ(d.version, GP_LINK_VERSION);
    CHECK_EQ(d.mode, GP_MODE_XBOX);
    CHECK_EQ(d.caps.features[1], 0x02);
    CHECK_EQ(d.caps.gamepad_type, 9);
    CHECK_EQ(d.caps.face_style, 0x13);
    CHECK_EQ(d.caps.accel_g, 4);
    CHECK_EQ(d.caps.gyro_dps, 2048);
    CHECK(!memcmp(d.caps.button_mask, h.caps.button_mask, 4));
    CHECK(!memcmp(d.serial, h.serial, 6));
    CHECK_EQ(d.tick_us, 3000);
}

static void fits_one_2dh1_packet(void)
{
    /* The state report's reason to exist: HIDP (1) + id (1) + L2CAP (4) +
     * payload fits a 2-DH1 packet's 54 bytes. */
    CHECK(GP_LINK_STATE_LEN + 1 + 1 + 4 <= 54);
}

int main(void)
{
    printf("test_link\n");
    RUN(state_round_trip);
    RUN(state_buttons_above_24_dropped);
    RUN(state_layout);
    RUN(short_packets_refused);
    RUN(status_round_trip);
    RUN(hello_round_trip);
    RUN(fits_one_2dh1_packet);
    return t_done("test_link");
}
