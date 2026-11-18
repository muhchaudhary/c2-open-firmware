/* gp_switch_desc.c: Switch mode's descriptors, protocol and reports (see
 * gp_switch.h), hardware-free, so the host tests build them. Layouts as
 * SDL's SDL_hidapi_switch.c and Linux's hid-nintendo read them, and as
 * dekuNukem's Nintendo_Switch_Reverse_Engineering documents them; the
 * descriptors, the SPI page and the NFC/IR reply as stock v3.56 has them
 * (fw-re/app_v356.bin: 0x5B0D0, 0x643EA, 0x640D0, 0x5F9EC). */
#include "system/includes.h"
#include "gp_switch.h"

/* USB: a real Pro Controller's (and stock's), 203 B. The joystick report
 * 0x30 it declares is not what the pad sends as 0x30 (hosts that know the
 * pad parse the bytes themselves); the rest is vendor reports of 63 B. */
const u8 sw_usb_report_desc[] = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0a, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0a, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0b, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0b, 0x01, 0x00, 0x01, 0x00, 0xa1, 0x00, 0x0b, 0x30, 0x00,
    0x01, 0x00, 0x0b, 0x31, 0x00, 0x01, 0x00, 0x0b, 0x32, 0x00, 0x01, 0x00, 0x0b, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xc0, 0x0b,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19, 0x0f, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x34, 0x81, 0x03, 0x06, 0x00, 0xff, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3f, 0x81, 0x03, 0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3f,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0x85, 0x80, 0x09, 0x05, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3f, 0x91, 0x83, 0xc0,
};
const u16 sw_usb_report_desc_len = sizeof(sw_usb_report_desc);

/* Bluetooth: 0x21 and 0x30 (48 B declared), 0x31..0x33 (NFC / IR, never
 * sent), 0x3F (plain HID: 16 buttons, hat, 4 axes), outputs 0x01, 0x10..0x12. */
const u8 sw_bt_report_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01, 0xff,
    0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x30, 0x09, 0x30, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02,
    0x85, 0x31, 0x09, 0x31, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
    0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
    0x85, 0x33, 0x09, 0x33, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02,
    0x85, 0x3f, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x05, 0x09, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34, 0x16, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02,
    0x06, 0x01, 0xff,
    0x85, 0x01, 0x09, 0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x10, 0x09, 0x10, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0x85, 0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02,
    0xc0,
};
const u16 sw_bt_report_desc_len = sizeof(sw_bt_report_desc);

/* ---- The SPI flash hosts read (subcommand 0x10) -----------------------------
 * 0x6000..0x60A9 as stock's emulated page: no serial (FF), device type 3
 * (Pro), colours in SPI (0x601B); stick calibration centre 0x800, range
 * 0x7FE each way (sw_stick12 encodes to it); body black, buttons white,
 * grips black (0x6050); the sticks' parameters with a dead zone of 0 (the
 * pad applies its own, gp_tuning.c). Changed from stock: the IMU's
 * (0x6020): origins 0, accel coefficient 0x4000 (hosts: 4 x LSB / coeff g,
 * so 4096 LSB/g: the pad halves SInput's 8192), gyro coefficient 14976
 * (936 x LSB / coeff °/s: 16 LSB/°/s, SInput's own unit, so no scaling).
 * Everything else reads FF: no user calibration at 0x8010 / 0x8026 (no
 * B2 A1 magic), so hosts take the factory one. */
#define SW_SPI_PAGE         0x6000
static const u8 sw_spi_6000[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   /* 0x6000 */
    0xff, 0xff, 0x03, 0x0a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0xff, 0xff, 0xff, 0xff,   /* 0x6010 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,   /* 0x6020 */
    0x00, 0x00, 0x80, 0x3a, 0x80, 0x3a, 0x80, 0x3a, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0xe7, 0x7f,   /* 0x6030 */
    0x00, 0x08, 0x80, 0xfe, 0xe7, 0x7f, 0x00, 0x08, 0x80, 0xfe, 0xe7, 0x7f, 0xfe, 0xe7, 0x7f, 0xff,   /* 0x6040 */
    0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,   /* 0x6050 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   /* 0x6060 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,   /* 0x6070 */
    0x50, 0xfd, 0x00, 0x00, 0xc6, 0x0f, 0x19, 0xd0, 0x4c, 0x00, 0x40, 0xe1, 0x00, 0x04, 0x40, 0x00,   /* 0x6080 */
    0x04, 0x40, 0x40, 0x0e, 0xe4, 0xc0, 0x01, 0x1c, 0x19, 0xd0, 0x4c, 0x00, 0x40, 0xe1, 0x00, 0x04,   /* 0x6090 */
    0x40, 0x00, 0x04, 0x40, 0x40, 0x0e, 0xe4, 0xc0, 0x01, 0x1c,                                       /* 0x60A0 */
};
#define SW_STICK_CENTRE     0x800
#define SW_STICK_RANGE      0x7FE

static u8 sw_spi_byte(u32 a)
{
    if (a >= SW_SPI_PAGE && a < SW_SPI_PAGE + sizeof(sw_spi_6000)) {
        return sw_spi_6000[a - SW_SPI_PAGE];
    }
    return 0xff;
}

/* ---- HD rumble ---------------------------------------------------------------
 * An actuator's 4 bytes: [1] bits 1..7 the high band's amplitude code
 * (0..0x64 after the shift), [3] 0x40 + half the low band's code, its odd
 * bit in [2] bit 7. Codes -> amplitude: SDL's table (SDL_hidapi_switch.c
 * hfa[], the amplitude / 257). Codes past the table are full. The two
 * motors from the bands, not the sides: sw_rumble_motors (gp_switch.h). */
static const u8 sw_amp[101] = {
      0,   2,   3,   4,   4,   5,   6,   7,   9,  10,  12,  14,  17,  20,  24,  29,
     30,  31,  33,  34,  36,  37,  39,  41,  42,  44,  46,  48,  50,  53,  55,  57,
     59,  60,  61,  63,  64,  65,  67,  68,  70,  71,  73,  74,  76,  78,  79,  81,
     83,  85,  87,  88,  90,  92,  94,  96,  99, 101, 103, 105, 108, 110, 112, 115,
    117, 120, 122, 125, 128, 131, 134, 136, 139, 142, 146, 149, 152, 155, 159, 162,
    166, 169, 173, 177, 181, 185, 189, 193, 197, 202, 206, 210, 215, 220, 225, 229,
    235, 240, 245, 250, 255,
};

static u8 sw_amp_code(int code)
{
    return code <= 0 ? 0 : code > 100 ? 255 : sw_amp[code];
}

void sw_rumble_bands(const u8 *r, u8 *high, u8 *low)
{
    *high = sw_amp_code(r[1] >> 1);
    *low = (r[3] & 0x7f) >= 0x40 ? sw_amp_code(((r[3] & 0x7f) - 0x40) * 2 + (r[2] >> 7)) : 0;
}

void sw_rumble_motors(const u8 *r8, u8 *big, u8 *small)
{
    u8 lh, ll, rh, rl;
    sw_rumble_bands(r8, &lh, &ll);
    sw_rumble_bands(r8 + 4, &rh, &rl);
    *big = ll;
    *small = rh;
    if (!rh && !rl) {
        *big = lh > ll ? lh : ll;                       /* the left side alone */
    }
    if (!lh && !ll) {
        *small = rh > rl ? rh : rl;                     /* the right side alone */
    }
}

/* ---- Helpers ---------------------------------------------------------------- */
static void put16(u8 *p, int v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

/* CRC-8, polynomial 0x07, initial 0 (the NFC/IR MCU's; stock's reply
 * carries 0x5C for its 33 bytes). */
static u8 sw_crc8(const u8 *p, int n)
{
    u8 c = 0;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c & 0x80) ? (u8)(c << 1) ^ 0x07 : (u8)(c << 1);
        }
    }
    return c;
}

static int sw_full(const struct sw_ctx *x)
{
    return (u8)(x->head - x->tail) >= SW_REPLIES;
}

/* A reply into the ring (the output's context). Full: dropped (a host
 * that sends 4 subcommands without waiting retries). */
static struct sw_reply *sw_queue(struct sw_ctx *x, u8 id)
{
    if (sw_full(x)) {
        return NULL;
    }
    struct sw_reply *r = &x->ring[x->head % SW_REPLIES];
    memset(r, 0, sizeof(*r));
    r->id = id;
    return r;
}

static void sw_commit(struct sw_ctx *x)
{
    x->head++;
}

/* A subcommand's reply: ack (0x80 | data type, or 0x80 bare), the
 * subcommand, n bytes of data. */
static void sw_ack(struct sw_ctx *x, u8 ack, u8 sub, const u8 *data, int n)
{
    struct sw_reply *r = sw_queue(x, SW_ID_REPLY);
    if (!r) {
        return;
    }
    r->data[0] = ack;
    r->data[1] = sub;
    if (n > 35) {
        n = 35;
    }
    if (n > 0) {
        memcpy(&r->data[2], data, n);
    }
    r->len = 37;
    sw_commit(x);
}

void sw_reset(struct sw_ctx *x, int usb, const u8 *mac)
{
    memset(x, 0, sizeof(*x));
    x->usb = usb;
    memcpy(x->mac, mac, 6);
    /* Bluetooth: plain HID until the host picks 0x30 (as a real one).
     * USB: nothing before the host's 80 04. */
    x->mode = usb ? 0 : SW_ID_SIMPLE;
}

/* ---- The host's output --------------------------------------------------------- */

/* Player lights (subcommand 0x30): the console's patterns for players
 * 1..4 (1, 3, 7, F), or one LED alone (SDL, hid-nintendo: 1 << n). The
 * high nibble (flashing: searching) shows as none. */
static u8 sw_player(u8 bits)
{
    switch (bits & 0x0f) {
    case 0x1: return 1;
    case 0x3: case 0x2: return 2;
    case 0x7: case 0x4: return 3;
    case 0xf: case 0x8: return 4;
    default: return 0;
    }
}

static void sw_subcommand(struct sw_ctx *x, u8 sub, const u8 *a, int n, struct sw_effect *e)
{
    u8 d[35];
    memset(d, 0, sizeof(d));
    switch (sub) {
    case 0x01:                                          /* Bluetooth pairing (USB): "done" */
        d[0] = 0x03;
        sw_ack(x, 0x81, sub, d, 1);
        break;
    case 0x02:                                          /* device info */
        d[0] = 0x04;                                    /* firmware 4.33, a recent Pro's */
        d[1] = 0x33;
        d[2] = 0x03;                                    /* Pro Controller */
        d[3] = 0x02;
        memcpy(&d[4], x->mac, 6);                       /* big-endian */
        d[10] = 0x01;
        d[11] = 0x01;                                   /* colours: from SPI */
        sw_ack(x, 0x82, sub, d, 12);
        break;
    case 0x03:                                          /* input report mode */
        if (n >= 1) {
            x->mode = a[0] == SW_ID_SIMPLE ? SW_ID_SIMPLE : SW_ID_STATE;   /* 0x31 (NFC/IR) as 0x30: there is none */
        }
        sw_ack(x, 0x80, sub, NULL, 0);
        break;
    case 0x04:                                          /* trigger buttons' elapsed time: none */
        sw_ack(x, 0x83, sub, d, 14);
        break;
    case 0x10: {                                        /* SPI flash read: address, length, data */
        if (n < 5) {
            sw_ack(x, 0x90, sub, d, 5);
            break;
        }
        u32 at = a[0] | a[1] << 8 | a[2] << 16 | (u32)a[3] << 24;
        u8 len = a[4] > 30 ? 30 : a[4];
        memcpy(d, a, 4);
        d[4] = len;
        for (int i = 0; i < len; i++) {
            d[5 + i] = sw_spi_byte(at + i);
        }
        sw_ack(x, 0x90, sub, d, 5 + len);
        break;
    }
    case 0x11:                                          /* SPI write, erase: "done" (not kept) */
    case 0x12:
        sw_ack(x, 0x80, sub, d, 1);
        break;
    case 0x21:                                          /* NFC/IR MCU configuration: stock's reply */
        d[0] = 0x01;
        d[2] = 0xff;
        d[4] = 0x03;
        d[6] = 0x05;
        d[7] = 0x01;
        d[33] = sw_crc8(d, 33);
        sw_ack(x, 0xa0, sub, d, 35);
        break;
    case 0x30:                                          /* player lights */
        if (n >= 1) {
            e->valid |= SW_FX_PLAYER;
            e->player = sw_player(a[0]);
        }
        sw_ack(x, 0x80, sub, NULL, 0);
        break;
    case 0x40:                                          /* IMU on / off */
        if (n >= 1) {
            x->imu_on = a[0] != 0;
            x->imu_n = 0;
        }
        sw_ack(x, 0x80, sub, NULL, 0);
        break;
    case 0x50:                                          /* regulated voltage: a mid value */
        put16(d, 1500);
        sw_ack(x, 0xd0, sub, d, 2);
        break;
    default:                                            /* 0x06, 0x08, 0x22, 0x38, 0x41, 0x48, ...: ack */
        sw_ack(x, 0x80, sub, NULL, 0);
        break;
    }
}

/* USB 0x80 xx: 01 status (type, address LSB first), 02 handshake, 03
 * 3 Mbit, 04 "HID only" (reports start, full mode), 05 back to the
 * timeout (stop), 06 reset. 0x92: a UART packet, its output report at +8. */
static void sw_usb_command(struct sw_ctx *x, const u8 *c, int len, struct sw_effect *e)
{
    struct sw_reply *r;
    switch (c[1]) {
    case 0x01:
        if ((r = sw_queue(x, SW_ID_USB_REPLY))) {
            r->data[0] = 0x01;
            r->data[1] = 0x00;
            r->data[2] = 0x03;                          /* Pro Controller */
            for (int k = 0; k < 6; k++) {
                r->data[3 + k] = x->mac[5 - k];
            }
            r->len = 9;
            sw_commit(x);
        }
        break;
    case 0x02:
    case 0x03:
        if ((r = sw_queue(x, SW_ID_USB_REPLY))) {
            r->data[0] = c[1];
            r->len = 1;
            sw_commit(x);
        }
        break;
    case 0x04:
        if (!x->mode) {
            x->mode = SW_ID_STATE;
        }
        break;
    case 0x05:
    case 0x06:
        x->mode = 0;
        break;
    case 0x92:
        if (len > 8) {
            sw_host_output(x, c + 8, len - 8, e);
        }
        break;
    }
}

int sw_host_output(struct sw_ctx *x, const u8 *c, int len, struct sw_effect *e)
{
    memset(e, 0, sizeof(*e));
    if (len < 2) {
        return 0;
    }
    switch (c[0]) {
    case SW_ID_RUMBLE_SUB:
    case SW_ID_RUMBLE:
        if (len < 10) {
            return 0;
        }
        e->valid |= SW_FX_RUMBLE;
        sw_rumble_motors(c + 2, &e->big, &e->small);
        if (c[0] == SW_ID_RUMBLE_SUB && len >= 11) {
            sw_subcommand(x, c[10], c + 11, len - 11, e);
        }
        return 1;
    case SW_ID_USB_CMD:
        sw_usb_command(x, c, len, e);
        return 1;
    default:
        return 0;
    }
}

/* ---- Input reports ----------------------------------------------------------------- */

static u8 stick8(s16 v)
{
    return (u16)(v + 32768) >> 8;
}

/* -32767..32767 -> centre +- range, 12 bits. up: the Switch's +Y is up. */
static u16 sw_stick12(s16 v, int up)
{
    s32 d = ((s32)v * SW_STICK_RANGE + (v < 0 ? -16383 : 16383)) / 32767;
    s32 o = SW_STICK_CENTRE + (up ? -d : d);
    return o < 0 ? 0 : o > 0xfff ? 0xfff : (u16)o;
}

static void sw_put_stick(u8 *p, s16 x, s16 y)
{
    u16 a = sw_stick12(x, 0), b = sw_stick12(y, 1);
    p[0] = (u8)a;
    p[1] = (u8)(a >> 8) | (u8)(b << 4);
    p[2] = (u8)(b >> 4);
}

/* Digital ZL / ZR: a quarter of the travel (after the profile's shaping),
 * or a button remapped to L2 / R2. */
static int sw_zl(const struct gp_state *s)
{
    return stick8(s->lt) >= 64 || (s->buttons & GP_BIT(GP_BTN_L2));
}

static int sw_zr(const struct gp_state *s)
{
    return stick8(s->rt) >= 64 || (s->buttons & GP_BIT(GP_BTN_R2));
}

/* Buttons by position, as DS4 mode's: the C2's bottom face button is the
 * Switch's B (its bottom one), the right one A, left Y, top X. Back is
 * Minus, Start Plus, Guide Home. Paddles: none (remap them). */
static const u8 sw_map[][2] = {                         /* gp button, byte * 8 + bit */
    { GP_BTN_WEST, 0 }, { GP_BTN_NORTH, 1 }, { GP_BTN_SOUTH, 2 }, { GP_BTN_EAST, 3 }, { GP_BTN_R1, 6 },
    { GP_BTN_BACK, 8 }, { GP_BTN_START, 9 }, { GP_BTN_R3, 10 }, { GP_BTN_L3, 11 },
    { GP_BTN_GUIDE, 12 }, { GP_BTN_CAPTURE, 13 },
    { GP_BTN_DOWN, 16 }, { GP_BTN_UP, 17 }, { GP_BTN_RIGHT, 18 }, { GP_BTN_LEFT, 19 }, { GP_BTN_L1, 22 },
};

static u32 sw_buttons(const struct gp_state *s)
{
    u32 bits = 0;
    for (int i = 0; i < ARRAY_SIZE(sw_map); i++) {
        if (s->buttons & GP_BIT(sw_map[i][0])) {
            bits |= 1u << sw_map[i][1];
        }
    }
    if (sw_zr(s)) {
        bits |= 1u << 7;
    }
    if (sw_zl(s)) {
        bits |= 1u << 23;
    }
    return bits;
}

/* Battery: level 8 full, 6, 4, 2 low, 0 empty; bit 0 charging. The low
 * nibble: bits 1..2 the kind (0: Pro), bit 0 powered (5 V). */
static u8 sw_battery(const struct gp_battery *b)
{
    u8 p = b->percent;
    u8 level = p >= 70 ? 8 : p >= 40 ? 6 : p >= 15 ? 4 : p >= 5 ? 2 : 0;
    if (b->charging) {
        level |= 1;
    }
    return level << 4 | (b->plug != 4 ? 1 : 0);
}

/* The 12 bytes every 0x21 / 0x30 starts with. */
static void sw_header(u8 *r, const struct gp_state *s, const struct gp_battery *b, u32 now_ms)
{
    r[0] = (u8)(now_ms / 5);                            /* a 5 ms timer, as the IMU's sample clock */
    r[1] = sw_battery(b);
    u32 bits = sw_buttons(s);
    r[2] = (u8)bits;
    r[3] = (u8)(bits >> 8);
    r[4] = (u8)(bits >> 16);
    sw_put_stick(&r[5], s->lx, s->ly);
    sw_put_stick(&r[8], s->rx, s->ry);
    r[11] = 0x80;                                       /* vibrator report */
}

/* SInput's frame (x left, y forward, z up; 8192 LSB/g, 16 LSB/°/s) to the
 * Switch's (x forward, y left, z up; 4096 LSB/g, 16 LSB/°/s with the SPI
 * page's coefficients). SDL reads the Switch's as (-Y, Z, -X) and SInput's
 * as (-X, Z, -Y): the same direction both ways. */
static void sw_imu_push(struct sw_ctx *x, const s16 *v)
{
    memmove(&x->imu[1], &x->imu[0], sizeof(x->imu[0]) * 2);
    x->imu[0][0] = v[1] / 2;
    x->imu[0][1] = v[0] / 2;
    x->imu[0][2] = v[2] / 2;
    x->imu[0][3] = v[4];
    x->imu[0][4] = v[3];
    x->imu[0][5] = v[5];
    if (x->imu_n < 3) {
        x->imu_n++;
    }
}

/* Three samples, newest first (SDL sends [2] first). Short of three
 * (the IMU just went on), the oldest one repeats. */
static void sw_put_imu(u8 *p, const struct sw_ctx *x)
{
    for (int k = 0; k < 3; k++) {
        int i = k < x->imu_n ? k : x->imu_n - 1;
        for (int a = 0; a < 6; a++) {
            put16(&p[12 * k + 2 * a], i < 0 ? 0 : x->imu[i][a]);
        }
    }
}

/* Plain HID 0x3F: buttons B A Y X L R ZL ZR, Minus Plus LS RS Home
 * Capture; the hat (0 up, clockwise, 8 none); LX LY RX RY u16 (centre
 * 0x8000, +Y down, as HID). */
static void sw_simple(u8 *r, const struct gp_state *s)
{
    static const u8 hat[16] = { 8, 0, 4, 8, 6, 7, 5, 8, 2, 1, 3, 8, 8, 8, 8, 8 };
    u32 b = s->buttons;
    r[0] = ((b & GP_BIT(GP_BTN_SOUTH)) ? 0x01 : 0) | ((b & GP_BIT(GP_BTN_EAST)) ? 0x02 : 0) |
           ((b & GP_BIT(GP_BTN_WEST)) ? 0x04 : 0) | ((b & GP_BIT(GP_BTN_NORTH)) ? 0x08 : 0) |
           ((b & GP_BIT(GP_BTN_L1)) ? 0x10 : 0) | ((b & GP_BIT(GP_BTN_R1)) ? 0x20 : 0) |
           (sw_zl(s) ? 0x40 : 0) | (sw_zr(s) ? 0x80 : 0);
    r[1] = ((b & GP_BIT(GP_BTN_BACK)) ? 0x01 : 0) | ((b & GP_BIT(GP_BTN_START)) ? 0x02 : 0) |
           ((b & GP_BIT(GP_BTN_L3)) ? 0x04 : 0) | ((b & GP_BIT(GP_BTN_R3)) ? 0x08 : 0) |
           ((b & GP_BIT(GP_BTN_GUIDE)) ? 0x10 : 0) | ((b & GP_BIT(GP_BTN_CAPTURE)) ? 0x20 : 0);
    u8 i = ((b & GP_BIT(GP_BTN_UP)) ? 1 : 0) | ((b & GP_BIT(GP_BTN_DOWN)) ? 2 : 0) |
           ((b & GP_BIT(GP_BTN_LEFT)) ? 4 : 0) | ((b & GP_BIT(GP_BTN_RIGHT)) ? 8 : 0);
    r[2] = hat[i];
    put16(&r[3], (u16)(s->lx + 32768));
    put16(&r[5], (u16)(s->ly + 32768));
    put16(&r[7], (u16)(s->rx + 32768));
    put16(&r[9], (u16)(s->ry + 32768));
}

void sw_reply_unsent(struct sw_ctx *x)
{
    x->resend = 1;
}

int sw_next_report(struct sw_ctx *x, const struct gp_state *s, const struct gp_battery *b,
                   u32 now_ms, u8 *id, u8 *rep)
{
    sw_imu_push(x, s->imu);
    memset(rep, 0, SW_REPORT_MAX);
    int full = x->usb ? SW_USB_LEN : SW_BT_LEN;
    const struct sw_reply *r = NULL;
    if (x->hold) {
        /* the caller's reply slot is taken: replies wait */
    } else if (x->resend) {
        x->resend = 0;
        r = &x->last;
    } else if (x->head != x->tail) {
        x->last = x->ring[x->tail % SW_REPLIES];
        x->tail++;
        r = &x->last;
    }
    if (r && r->id == SW_ID_USB_REPLY) {
        *id = SW_ID_USB_REPLY;
        memcpy(rep, r->data, r->len);
        return SW_USB_LEN;
    }
    if (r) {
        *id = SW_ID_REPLY;
        sw_header(rep, s, b, now_ms);
        memcpy(&rep[12], r->data, r->len);
        return full;
    }
    u32 period = x->usb ? SW_USB_PERIOD_MS : SW_BT_PERIOD_MS;
    if (!x->mode || now_ms - x->state_ms < period) {
        return 0;
    }
    if (x->mode == SW_ID_SIMPLE) {
        /* As a real one: on a change, and now and then. */
        sw_simple(rep, s);
        if (!memcmp(rep, x->simple_last, SW_SIMPLE_LEN) && now_ms - x->state_ms < 500) {
            memset(rep, 0, SW_SIMPLE_LEN);
            return 0;
        }
        memcpy(x->simple_last, rep, SW_SIMPLE_LEN);
        x->state_ms = now_ms;
        *id = SW_ID_SIMPLE;
        return x->usb ? SW_USB_LEN : SW_SIMPLE_LEN;
    }
    x->state_ms = now_ms;
    *id = SW_ID_STATE;
    sw_header(rep, s, b, now_ms);
    if (x->imu_on) {
        sw_put_imu(&rep[12], x);
    }
    return full;
}
