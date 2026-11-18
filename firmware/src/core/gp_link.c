/* gp_link.c: see gp_link.h. Little-endian throughout. */
#include "gp_link.h"
#include <string.h>

static void put16(u8 *p, int v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static u16 get16(const u8 *p)
{
    return p[0] | p[1] << 8;
}

/* [0] seq  [1..3] buttons  [4..11] LX LY RX RY  [12..15] LT RT
 * [16..27] accel X Y Z, gyro X Y Z  [28..29] ms */
void gp_link_encode_state(u8 *p, u8 seq, const struct gp_state *s, u16 ms)
{
    p[0] = seq;
    p[1] = (u8)s->buttons;
    p[2] = (u8)(s->buttons >> 8);
    p[3] = (u8)(s->buttons >> 16);
    put16(p + 4, s->lx);
    put16(p + 6, s->ly);
    put16(p + 8, s->rx);
    put16(p + 10, s->ry);
    put16(p + 12, s->lt);
    put16(p + 14, s->rt);
    for (int i = 0; i < 6; i++) {
        put16(p + 16 + 2 * i, s->imu[i]);
    }
    put16(p + 28, ms);
}

int gp_link_decode_state(const u8 *p, int len, u8 *seq, struct gp_state *s, u16 *ms)
{
    if (len < GP_LINK_STATE_LEN) {
        return 0;
    }
    *seq = p[0];
    s->buttons = p[1] | p[2] << 8 | (u32)p[3] << 16;
    s->lx = (s16)get16(p + 4);
    s->ly = (s16)get16(p + 6);
    s->rx = (s16)get16(p + 8);
    s->ry = (s16)get16(p + 10);
    s->lt = (s16)get16(p + 12);
    s->rt = (s16)get16(p + 14);
    for (int i = 0; i < 6; i++) {
        s->imu[i] = (s16)get16(p + 16 + 2 * i);
    }
    *ms = get16(p + 28);
    return 1;
}

/* [0] plug  [1] percent  [2] charging  [3..4] mV */
void gp_link_encode_status(u8 *p, const struct gp_battery *b)
{
    p[0] = b->plug;
    p[1] = b->percent;
    p[2] = b->charging;
    put16(p + 3, b->mv);
}

int gp_link_decode_status(const u8 *p, int len, struct gp_battery *b)
{
    if (len < GP_LINK_STATUS_LEN) {
        return 0;
    }
    b->plug = p[0];
    b->percent = p[1];
    b->charging = p[2];
    b->mv = get16(p + 3);
    return 1;
}

/* [0] version  [1] mode  [2..3] features  [4] type  [5] face  [6..7] accel g
 * [8..9] gyro dps  [10..13] button mask  [14..19] serial  [20..21] tick us */
void gp_link_encode_hello(u8 *p, const struct gp_link_hello *h)
{
    p[0] = h->version;
    p[1] = h->mode;
    p[2] = h->caps.features[0];
    p[3] = h->caps.features[1];
    p[4] = h->caps.gamepad_type;
    p[5] = h->caps.face_style;
    put16(p + 6, h->caps.accel_g);
    put16(p + 8, h->caps.gyro_dps);
    memcpy(p + 10, h->caps.button_mask, 4);
    memcpy(p + 14, h->serial, 6);
    put16(p + 20, h->tick_us);
}

int gp_link_decode_hello(const u8 *p, int len, struct gp_link_hello *h)
{
    if (len < GP_LINK_HELLO_LEN) {
        return 0;
    }
    h->version = p[0];
    h->mode = p[1];
    h->caps.features[0] = p[2];
    h->caps.features[1] = p[3];
    h->caps.gamepad_type = p[4];
    h->caps.face_style = p[5];
    h->caps.accel_g = get16(p + 6);
    h->caps.gyro_dps = get16(p + 8);
    memcpy(h->caps.button_mask, p + 10, 4);
    memcpy(h->serial, p + 14, 6);
    h->tick_us = get16(p + 20);
    return 1;
}
