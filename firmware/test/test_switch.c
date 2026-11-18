/* test_switch.c: Switch mode's protocol (core/gp_switch_desc.c), against
 * what SDL's SDL_hidapi_switch.c and Linux's hid-nintendo read: the USB
 * handshake, subcommand replies (device info, SPI reads, the NFC/IR
 * reply's CRC-8), the 0x30 / 0x3F layouts, rumble decoding, pacing. */
#include <string.h>
#include "test.h"
#include "gp_switch.h"

static const u8 mac[6] = { 0x98, 0xb6, 0xe9, 0x11, 0x22, 0x33 };   /* MSB first */
static const struct gp_battery on_battery = { .percent = 55, .plug = 4 };
static const struct gp_state idle = { .lt = -32768, .rt = -32768 };

static struct sw_ctx x;
static u8 rep[SW_REPORT_MAX], id;
static u32 now;

static u16 get16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

/* One tick: the report's length after the ID (0: none). */
static int tick(const struct gp_state *s)
{
    return sw_next_report(&x, s, &on_battery, now, &id, rep);
}

/* A host output report, the effect discarded. */
static int out(const u8 *c, int len)
{
    struct sw_effect e;
    return sw_host_output(&x, c, len, &e);
}

/* Subcommand sub with args, neutral rumble; returns the effect. */
static struct sw_effect subcmd(u8 sub, const u8 *args, int n)
{
    static u8 pkt_no;
    u8 c[49] = { SW_ID_RUMBLE_SUB, pkt_no++, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, sub };
    if (n) {
        memcpy(&c[11], args, n);
    }
    struct sw_effect e;
    CHECK(sw_host_output(&x, c, sizeof(c), &e));
    return e;
}

/* ---- Report sizes per ID, as test_ds4's parser. */
struct hid_sizes {
    u32 in_bits[256], out_bits[256];
    int depth, ok;
};
static struct hid_sizes r;

static void hid_parse(const u8 *d, int len)
{
    memset(&r, 0, sizeof(r));
    r.ok = 1;
    u32 size = 0, count = 0;
    int rid = 0;
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
        case 0x84: rid = v & 0xff; break;
        case 0x80: r.in_bits[rid] += size * count; break;
        case 0x90: r.out_bits[rid] += size * count; break;
        case 0xa0: r.depth++; break;
        case 0xc0: r.depth--; break;
        }
        i += 1 + n;
    }
}

static void descriptors(void)
{
    CHECK_EQ(sw_usb_report_desc_len, 203);              /* a real Pro Controller's */
    hid_parse(sw_usb_report_desc, sw_usb_report_desc_len);
    CHECK(r.ok);
    CHECK_EQ(r.depth, 0);
    CHECK_EQ(r.in_bits[SW_ID_STATE], 63 * 8);
    CHECK_EQ(r.in_bits[SW_ID_REPLY], 63 * 8);
    CHECK_EQ(r.in_bits[SW_ID_USB_REPLY], 63 * 8);
    CHECK_EQ(r.out_bits[SW_ID_RUMBLE_SUB], 63 * 8);
    CHECK_EQ(r.out_bits[SW_ID_RUMBLE], 63 * 8);
    CHECK_EQ(r.out_bits[SW_ID_USB_CMD], 63 * 8);

    hid_parse(sw_bt_report_desc, sw_bt_report_desc_len);
    CHECK(r.ok);
    CHECK_EQ(r.depth, 0);
    CHECK_EQ(r.in_bits[SW_ID_STATE], 48 * 8);
    CHECK_EQ(r.in_bits[SW_ID_REPLY], 48 * 8);
    CHECK_EQ(r.in_bits[SW_ID_SIMPLE], SW_SIMPLE_LEN * 8);
    CHECK_EQ(r.out_bits[SW_ID_RUMBLE_SUB], 48 * 8);
    CHECK_EQ(r.out_bits[SW_ID_RUMBLE], 48 * 8);
    CHECK(sw_bt_report_desc_len <= 334);                /* the SDP record has room for Xbox's 334 B */
}

/* ---- USB: nothing until the handshake's 80 04; 80 01 / 02 / 03 answered. */
static void usb_handshake(void)
{
    sw_reset(&x, 1, mac);
    now = 1000;
    CHECK_EQ(tick(&idle), 0);
    const u8 status[2] = { 0x80, 0x01 };
    CHECK(out(status, 2));
    CHECK_EQ(tick(&idle), SW_USB_LEN);
    CHECK_EQ(id, SW_ID_USB_REPLY);
    CHECK_EQ(rep[0], 0x01);
    CHECK_EQ(rep[2], 0x03);                             /* Pro Controller (SDL: ucDeviceType) */
    for (int k = 0; k < 6; k++) {
        CHECK_EQ(rep[3 + k], mac[5 - k]);               /* LSB first (SDL reverses it) */
    }
    for (u8 c = 0x02; c <= 0x03; c++) {
        const u8 cmd[2] = { 0x80, c };
        out(cmd, 2);
        CHECK_EQ(tick(&idle), SW_USB_LEN);
        CHECK_EQ(id, SW_ID_USB_REPLY);
        CHECK_EQ(rep[0], c);
    }
    CHECK_EQ(tick(&idle), 0);                           /* still no state */
    const u8 hid_only[2] = { 0x80, 0x04 };
    out(hid_only, 2);
    CHECK_EQ(tick(&idle), SW_USB_LEN);                  /* 80 04: no reply, the full state at once */
    CHECK_EQ(id, SW_ID_STATE);
    CHECK_EQ(tick(&idle), 0);                           /* not before the period */
    now += SW_USB_PERIOD_MS - 1;
    CHECK_EQ(tick(&idle), 0);
    now += 1;
    CHECK_EQ(tick(&idle), SW_USB_LEN);
    /* A UART-wrapped (80 92) subcommand reaches the protocol too. */
    u8 wrapped[8 + 12] = { 0x80, 0x92, 0x00, 0x31, 0, 0, 0, 0, SW_ID_RUMBLE_SUB, 0, 0, 1, 0x40, 0x40, 0, 1, 0x40, 0x40, 0x02 };
    out(wrapped, sizeof(wrapped));
    CHECK_EQ(tick(&idle), SW_USB_LEN);
    CHECK_EQ(id, SW_ID_REPLY);
    CHECK_EQ(rep[12], 0x82);
}

/* ---- Bluetooth: plain HID first, 0x30 after subcommand 03 30. */
static void bt_modes(void)
{
    sw_reset(&x, 0, mac);
    now = 5000;
    CHECK_EQ(tick(&idle), SW_SIMPLE_LEN);
    CHECK_EQ(id, SW_ID_SIMPLE);
    CHECK_EQ(rep[2], 8);                                /* hat released */
    CHECK_EQ(get16(&rep[3]), 0x8000);                   /* centred */
    now += SW_BT_PERIOD_MS;
    CHECK_EQ(tick(&idle), 0);                           /* unchanged: not again yet */
    struct gp_state a = idle;
    a.buttons = GP_BIT(GP_BTN_EAST) | GP_BIT(GP_BTN_UP);
    CHECK_EQ(tick(&a), SW_SIMPLE_LEN);                  /* a change: at once */
    CHECK_EQ(rep[0], 0x02);                             /* A */
    CHECK_EQ(rep[2], 0);                                /* hat up */
    now += 500;
    CHECK_EQ(tick(&a), SW_SIMPLE_LEN);                  /* and now and then */

    const u8 full = 0x30;
    subcmd(0x03, &full, 1);
    CHECK_EQ(tick(&idle), SW_BT_LEN);                   /* the reply first, at once */
    CHECK_EQ(id, SW_ID_REPLY);
    CHECK_EQ(rep[12], 0x80);
    CHECK_EQ(rep[13], 0x03);
    now += SW_BT_PERIOD_MS;
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(id, SW_ID_STATE);
    now += SW_BT_PERIOD_MS - 1;
    CHECK_EQ(tick(&idle), 0);                           /* 15 ms apart */
}

static void device_info(void)
{
    sw_reset(&x, 0, mac);
    subcmd(0x02, NULL, 0);
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(id, SW_ID_REPLY);
    CHECK_EQ(rep[12], 0x82);
    CHECK_EQ(rep[13], 0x02);
    CHECK_EQ(rep[14 + 2], 0x03);                        /* Pro Controller */
    CHECK(!memcmp(&rep[14 + 4], mac, 6));               /* big-endian */
    CHECK_EQ(rep[14 + 11], 0x01);                       /* colours from SPI */
}

/* SPI read: the address and length echoed, then the bytes. */
static const u8 *spi(u32 at, u8 len)
{
    u8 a[5] = { at, at >> 8, at >> 16, at >> 24, len };
    subcmd(0x10, a, 5);
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(rep[12], 0x90);
    CHECK_EQ(rep[13], 0x10);
    CHECK(!memcmp(&rep[14], a, 5));
    return &rep[19];
}

static void spi_reads(void)
{
    sw_reset(&x, 0, mac);
    const u8 *d = spi(0x8010, 0x16);                    /* user stick calibration: none */
    CHECK_EQ(d[0], 0xff);
    CHECK_EQ(d[1], 0xff);
    d = spi(0x603D, 0x12);                              /* factory stick calibration */
    /* left: above, centre, below; right: centre, below, above (SDL) */
    u16 lcx = d[3] | (d[4] & 0x0f) << 8, lcy = d[4] >> 4 | d[5] << 4;
    u16 lax = d[0] | (d[1] & 0x0f) << 8, rcx = d[9] | (d[10] & 0x0f) << 8;
    CHECK_EQ(lcx, 0x800);
    CHECK_EQ(lcy, 0x800);
    CHECK_EQ(lax, 0x7fe);
    CHECK_EQ(rcx, 0x800);
    d = spi(0x6020, 0x18);                              /* IMU: origins 0, coefficients */
    CHECK_EQ(get16(&d[0]), 0);
    CHECK_EQ(get16(&d[6]), 0x4000);                     /* 4 x 4096 LSB/g */
    CHECK_EQ(get16(&d[18]), 14976);                     /* 936 x 16 LSB/°/s */
    d = spi(0x6050, 12);                                /* colours: body, buttons */
    CHECK_EQ(d[0], 0x00);
    CHECK_EQ(d[3], 0xff);
    d = spi(0x6086, 18);                                /* stick parameters: dead zone 0 */
    CHECK_EQ(d[3] | (d[4] & 0x0f) << 8, 0);
    d = spi(0x60A8, 4);                                 /* past the page's end: FF */
    CHECK_EQ(d[1], 0x1c);
    CHECK_EQ(d[2], 0xff);
}

static u8 crc8(const u8 *p, int n)
{
    u8 c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = c & 0x80 ? (u8)(c << 1) ^ 7 : (u8)(c << 1);
        }
    }
    return c;
}

static void mcu_config(void)
{
    sw_reset(&x, 0, mac);
    const u8 a[3] = { 0x21, 0x00, 0x00 };
    subcmd(0x21, a, 3);
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(rep[12], 0xa0);
    CHECK_EQ(rep[14], 0x01);
    CHECK_EQ(rep[18], 0x03);
    CHECK_EQ(rep[47], 0x5c);                            /* stock's CRC byte, at report[48] with the ID */
    CHECK_EQ(rep[47], crc8(&rep[14], 33));
}

/* ---- The full state's layout. */
static void state_layout(void)
{
    sw_reset(&x, 0, mac);
    const u8 full = 0x30, on = 1;
    subcmd(0x03, &full, 1);
    tick(&idle);
    struct gp_state s = idle;
    s.buttons = GP_BIT(GP_BTN_SOUTH) | GP_BIT(GP_BTN_START) | GP_BIT(GP_BTN_GUIDE) | GP_BIT(GP_BTN_LEFT) |
                GP_BIT(GP_BTN_L1);
    s.lx = 32767;                                       /* full right */
    s.ly = -32767;                                      /* full up (gp: +Y down) */
    s.rx = -32767;
    s.rt = 32767;                                       /* ZR */
    now += 100;
    CHECK_EQ(tick(&s), SW_BT_LEN);
    CHECK_EQ(id, SW_ID_STATE);
    CHECK_EQ(rep[1], 0x60);                             /* 55 %: level 6, on battery, a Pro */
    CHECK_EQ(rep[2], 0x04 | 0x80);                      /* B (bottom), ZR */
    CHECK_EQ(rep[3], 0x02 | 0x10);                      /* Plus, Home */
    CHECK_EQ(rep[4], 0x08 | 0x40);                      /* Left, L */
    u16 lx = rep[5] | (rep[6] & 0x0f) << 8, ly = rep[6] >> 4 | rep[7] << 4;
    u16 rx = rep[8] | (rep[9] & 0x0f) << 8, ry = rep[9] >> 4 | rep[10] << 4;
    CHECK_EQ(lx, 0x800 + 0x7fe);
    CHECK_EQ(ly, 0x800 + 0x7fe);                        /* the Switch's +Y is up */
    CHECK_EQ(rx, 0x800 - 0x7fe);
    CHECK_EQ(ry, 0x800);
    for (int i = 12; i < 48; i++) {
        CHECK_EQ(rep[i], 0);                            /* IMU off: no samples */
    }

    /* IMU on: three samples, newest first, the Switch's frame and units. */
    subcmd(0x40, &on, 1);
    tick(&idle);
    for (int k = 0; k < 4; k++) {
        struct gp_state m = idle;
        m.imu[0] = 100 * k;                             /* SInput accel x (left) */
        m.imu[1] = 1000 + k;                            /* y (forward) */
        m.imu[2] = 8192;                                /* z: 1 g */
        m.imu[3] = 16 * k;                              /* gyro x */
        m.imu[4] = 32;
        m.imu[5] = -48;
        now += 4;
        tick(&m);
    }
    now += SW_BT_PERIOD_MS;
    struct gp_state m = idle;
    m.imu[0] = 400;
    m.imu[1] = 1004;
    m.imu[2] = 8192;
    m.imu[3] = 64;
    m.imu[4] = 32;
    m.imu[5] = -48;
    CHECK_EQ(tick(&m), SW_BT_LEN);
    CHECK_EQ(id, SW_ID_STATE);
    CHECK_EQ((s16)get16(&rep[12]), 1004 / 2);           /* Switch x = SInput y, halved */
    CHECK_EQ((s16)get16(&rep[14]), 400 / 2);            /* Switch y = SInput x */
    CHECK_EQ((s16)get16(&rep[16]), 4096);               /* 1 g */
    CHECK_EQ((s16)get16(&rep[18]), 32);                 /* gyro x = SInput's y, as it is */
    CHECK_EQ((s16)get16(&rep[20]), 64);
    CHECK_EQ((s16)get16(&rep[22]), -48);
    CHECK_EQ((s16)get16(&rep[14 + 12]), 300 / 2);       /* the one before */
    CHECK_EQ((s16)get16(&rep[14 + 24]), 200 / 2);       /* the oldest */
}

/* ---- Rumble: SDL's encoding (EncodeRumble) decoded. */
static void encode_rumble(u8 *r, int hf, int lf)
{
    if (!hf && !lf) {                                   /* SDL: neutral */
        r[0] = 0x00;
        r[1] = 0x01;
        r[2] = 0x40;
        r[3] = 0x40;
        return;
    }
    u16 lfa = (0x40 + lf / 2) | (lf & 1 ? 0x8000 : 0);
    r[0] = 0x74;                                        /* 320 Hz high band, a typical one */
    r[1] = 0x00 + 2 * hf;
    r[2] = 0x3d | ((lfa >> 8) & 0x80);                  /* 160 Hz low band */
    r[3] = lfa & 0xff;
}

static void rumble(void)
{
    const u8 neutral[4] = { 0x00, 0x01, 0x40, 0x40 };
    u8 hi, lo;
    sw_rumble_bands(neutral, &hi, &lo);
    CHECK_EQ(hi, 0);
    CHECK_EQ(lo, 0);
    u8 r[4];
    int last = -1;
    for (int i = 0; i <= 100; i++) {
        encode_rumble(r, i, i);
        sw_rumble_bands(r, &hi, &lo);
        CHECK(hi >= last);
        CHECK_EQ(lo, hi);                               /* the same code, both bands */
        last = hi;
    }
    CHECK_EQ(last, 255);
    encode_rumble(r, 64, 0);                            /* SDL 30134 / 65535 */
    sw_rumble_bands(r, &hi, &lo);
    CHECK_NEAR(hi, 30134 / 257, 1);
    CHECK_EQ(lo, 0);

    /* The motors. SDL: its low / high channels as the bands, the same on
     * both actuators -> big = low, small = high (2026-11-18: with the
     * stronger band per side instead, SDL's high ran the big motor too). */
    u8 r8[8], big, small;
    encode_rumble(r8, 0, 100);                          /* SDL low only */
    encode_rumble(r8 + 4, 0, 100);
    sw_rumble_motors(r8, &big, &small);
    CHECK_EQ(big, 255);
    CHECK_EQ(small, 0);
    encode_rumble(r8, 100, 0);                          /* SDL high only */
    encode_rumble(r8 + 4, 100, 0);
    sw_rumble_motors(r8, &big, &small);
    CHECK_EQ(big, 0);
    CHECK_EQ(small, 255);
    encode_rumble(r8, 64, 100);                         /* both */
    encode_rumble(r8 + 4, 64, 100);
    sw_rumble_motors(r8, &big, &small);
    CHECK_EQ(big, 255);
    CHECK_NEAR(small, 30134 / 257, 1);
    /* hid-nintendo: strong -> left, weak -> right, both bands alike. */
    encode_rumble(r8, 100, 100);
    memcpy(r8 + 4, neutral, 4);
    sw_rumble_motors(r8, &big, &small);
    CHECK_EQ(big, 255);
    CHECK_EQ(small, 0);
    memcpy(r8, neutral, 4);
    encode_rumble(r8 + 4, 64, 64);
    sw_rumble_motors(r8, &big, &small);
    CHECK_EQ(big, 0);
    CHECK_NEAR(small, 30134 / 257, 1);
    /* one side's high band alone still rumbles its motor */
    encode_rumble(r8, 100, 0);
    memcpy(r8 + 4, neutral, 4);
    sw_rumble_motors(r8, &big, &small);
    CHECK_EQ(big, 255);

    sw_reset(&x, 0, mac);
    u8 c[10] = { SW_ID_RUMBLE, 0 };
    encode_rumble(&c[2], 0, 100);
    encode_rumble(&c[6], 0, 100);
    struct sw_effect e;
    CHECK(sw_host_output(&x, c, sizeof(c), &e));
    CHECK_EQ(e.valid, SW_FX_RUMBLE);
    CHECK_EQ(e.big, 255);
    CHECK_EQ(e.small, 0);
    CHECK_EQ(tick(&idle), SW_SIMPLE_LEN);               /* rumble alone: no reply */
    CHECK_EQ(id, SW_ID_SIMPLE);
}

static void player_lights(void)
{
    sw_reset(&x, 0, mac);
    static const u8 pat[][2] = { { 0x01, 1 }, { 0x03, 2 }, { 0x07, 3 }, { 0x0f, 4 }, { 0x04, 3 }, { 0xf0, 0 } };
    for (int i = 0; i < (int)(sizeof(pat) / sizeof(pat[0])); i++) {
        struct sw_effect e = subcmd(0x30, &pat[i][0], 1);
        CHECK(e.valid & SW_FX_PLAYER);
        CHECK_EQ(e.player, pat[i][1]);
        tick(&idle);
    }
}

/* ---- The reply ring: full drops, unsent goes again. */
static void replies(void)
{
    sw_reset(&x, 0, mac);
    for (int i = 0; i < SW_REPLIES + 2; i++) {
        u8 m = 0x30;
        subcmd(i == 0 ? 0x02 : 0x03, &m, 1);
    }
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(rep[13], 0x02);
    sw_reply_unsent(&x);
    CHECK_EQ(tick(&idle), SW_BT_LEN);                   /* the same again */
    CHECK_EQ(rep[13], 0x02);
    int n = 0;
    while (tick(&idle) && id == SW_ID_REPLY) {
        n++;
    }
    CHECK_EQ(n, SW_REPLIES - 1);                        /* the two past a full ring were dropped */
    CHECK(!out((const u8 *)"\x05\x00", 2));             /* not a Switch report */
    /* hold: replies wait, the state goes on */
    const u8 full = 0x30;
    subcmd(0x03, &full, 1);
    x.hold = 1;
    now += 100;
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(id, SW_ID_STATE);
    x.hold = 0;
    CHECK_EQ(tick(&idle), SW_BT_LEN);
    CHECK_EQ(id, SW_ID_REPLY);
}

int main(void)
{
    printf("switch\n");
    RUN(descriptors);
    RUN(usb_handshake);
    RUN(bt_modes);
    RUN(device_info);
    RUN(spi_reads);
    RUN(mcu_config);
    RUN(state_layout);
    RUN(rumble);
    RUN(player_lights);
    RUN(replies);
    return t_done("switch");
}
