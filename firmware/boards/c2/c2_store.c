/*
 * c2_store.c: the C2's record store (core/gp_store.h) on its I2C EEPROM
 * (0xA0, 8 KB, 32-byte pages; drivers/eeprom24.c). Replaces the default
 * syscfg store; falls back to it when the EEPROM doesn't answer.
 *
 * Layout (ours; stock's is in docs/stock-features.md, and going back to
 * stock means restoring backups/c2-pad-eeprom.bin as well):
 *
 *   0x0000  header: "C2EE", layout version, 0, CRC-16 of the 6 bytes before
 *   0x0040  FACTORY record (one copy): stock's factory calibration, copied
 *           at format time from 0x1700 (66 B, sticks + triggers) and 0x1820
 *           (14 B, IMU offsets), payload [mask][66][14]
 *   0x0100  CAL record, copies A 0x0100 / B 0x0180 (128 B each)
 *   0x0200  SETTINGS record, copies A 0x0200 / B 0x0600 (1 KB each)
 *   0x0A00.. free. Stock's 0x1700 / 0x1820 are left as they are.
 *
 * A record: 8-byte header ['R', id, seq (u16 LE), len (u16 LE), CRC-16
 * (u16 LE) over id..len and the payload], then the payload. Two copies:
 * a read takes the valid one with the newer seq, a write goes to the other
 * one (payload first, header last), so a power cut mid-write leaves the
 * previous record. The EEPROM driver skips pages that already hold the
 * bytes, so an unchanged record costs one header page.
 *
 * Format (no valid header: first boot of this layout): the factory
 * calibration copied, the CAL and SETTINGS records copied from syscfg (the
 * values saved before this layout), every copy B invalidated, the header
 * last. Until the header is written, the next boot formats again.
 */
#include "system/includes.h"
#include "app_config.h"

#ifdef CONFIG_BOARD_C2

#include "syscfg_id.h"
#include "gamepad.h"
#include "gp_log.h"
#include "gp_store.h"
#include "jl_iic.h"
#include "eeprom24.h"

extern const struct eeprom24 c2_eeprom;                  /* gamepad_c2.c */

#define ST_LAYOUT           1
#define ST_HDR              0x0000
#define ST_FACTORY          0x0040
#define ST_FACTORY_SLOT     0x00C0
#define ST_REC_HDR          8
#define ST_REC_MAGIC        'R'
#define ST_ID_FACTORY       0xF0

/* Stock's factory records (docs/stock-features.md). */
#define STOCK_CAL           0x1700      /* 66 B, magic A6 6A at [64] */
#define STOCK_CAL_LEN       66
#define STOCK_IMU           0x1820      /* 14 B, magic 68 67 at [12] */
#define STOCK_IMU_LEN       14

struct st_slot {
    u8 id;
    u16 a, b, size;                     /* copy addresses, bytes per copy */
};
static const struct st_slot st_slots[] = {
    { GP_STORE_CAL,      0x0100, 0x0180, 0x0080 },
    { GP_STORE_SETTINGS, 0x0200, 0x0600, 0x0400 },
};

static u8 st_state;                     /* 0 not yet, 1 EEPROM in use, 2 syscfg (no EEPROM) */

static u16 crc16(u16 crc, const u8 *p, int n)
{
    while (n--) {
        crc ^= (u16)*p++ << 8;
        for (int i = 0; i < 8; i++) {
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

static const struct st_slot *st_find(u8 id)
{
    for (int i = 0; i < ARRAY_SIZE(st_slots); i++) {
        if (st_slots[i].id == id) {
            return &st_slots[i];
        }
    }
    return NULL;
}

/* A copy at addr: valid if its header and CRC hold (the payload is read in
 * 32-byte pieces to check it). Returns 1 with seq / len filled in. */
static int st_copy_valid(u8 id, u16 addr, u16 room, u16 *seq, u16 *len)
{
    u8 h[ST_REC_HDR];
    if (eeprom24_read(&c2_eeprom, addr, h, sizeof(h)) || h[0] != ST_REC_MAGIC || h[1] != id) {
        return 0;
    }
    *seq = h[2] | h[3] << 8;
    *len = h[4] | h[5] << 8;
    if (*len > room - ST_REC_HDR) {
        return 0;
    }
    u16 crc = crc16(0xFFFF, h + 1, 5);
    u8 b[32];
    for (u16 at = 0; at < *len; at += sizeof(b)) {
        u16 n = MIN(sizeof(b), *len - at);
        if (eeprom24_read(&c2_eeprom, addr + ST_REC_HDR + at, b, n)) {
            return 0;
        }
        crc = crc16(crc, b, n);
    }
    return crc == (h[6] | h[7] << 8);
}

/* The newest valid copy of a record: its address (0 = none), seq and len. */
static u16 st_newest(const struct st_slot *s, u16 *seq, u16 *len)
{
    u16 sa, la, sb, lb;
    int va = st_copy_valid(s->id, s->a, s->size, &sa, &la);
    int vb = st_copy_valid(s->id, s->b, s->size, &sb, &lb);
    if (va && (!vb || (s16)(sa - sb) > 0)) {
        *seq = sa;
        *len = la;
        return s->a;
    }
    if (vb) {
        *seq = sb;
        *len = lb;
        return s->b;
    }
    return 0;
}

static int st_write_copy(u8 id, u16 addr, u16 seq, const void *buf, u16 len)
{
    u8 h[ST_REC_HDR] = { ST_REC_MAGIC, id, seq, seq >> 8, len, len >> 8 };
    u16 crc = crc16(crc16(0xFFFF, h + 1, 5), buf, len);
    h[6] = crc;
    h[7] = crc >> 8;
    int r = eeprom24_write(&c2_eeprom, addr + ST_REC_HDR, buf, len);
    if (!r) {
        r = eeprom24_write(&c2_eeprom, addr, h, sizeof(h));
    }
    return r;
}

static int st_invalidate(u16 addr)
{
    static const u8 ff[ST_REC_HDR] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    return eeprom24_write(&c2_eeprom, addr, ff, sizeof(ff));
}

/* Stock's factory calibration into our FACTORY record: [mask][66][14]; mask
 * bit 0 = the stick/trigger record had its magic, bit 1 = the IMU's. */
static int st_copy_factory(u8 *mask_out)
{
    u8 p[1 + STOCK_CAL_LEN + STOCK_IMU_LEN];
    memset(p, 0xff, sizeof(p));
    p[0] = 0;
    if (!eeprom24_read(&c2_eeprom, STOCK_CAL, p + 1, STOCK_CAL_LEN) &&
        p[1 + 64] == 0xA6 && p[1 + 65] == 0x6A) {
        p[0] |= 1;
    }
    if (!eeprom24_read(&c2_eeprom, STOCK_IMU, p + 1 + STOCK_CAL_LEN, STOCK_IMU_LEN) &&
        p[1 + STOCK_CAL_LEN + 12] == 0x68 && p[1 + STOCK_CAL_LEN + 13] == 0x67) {
        p[0] |= 2;
    }
    *mask_out = p[0];
    return st_write_copy(ST_ID_FACTORY, ST_FACTORY, 1, p, sizeof(p));
}

static int st_format(void)
{
    u8 mask = 0;
    int r = st_copy_factory(&mask);
    char moved[40] = "";
    u8 *buf = malloc(0x400);                             /* the biggest record; once */
    if (!buf) {
        r = -1;
    }
    for (int i = 0; !r && i < ARRAY_SIZE(st_slots); i++) {
        const struct st_slot *s = &st_slots[i];
        int n = syscfg_read(s->id, buf, s->size - ST_REC_HDR);
        if (n > 0) {
            r = st_write_copy(s->id, s->a, 1, buf, n);
            strcat(moved, s->id == GP_STORE_CAL ? " cal" : " settings");
        } else {
            r = st_invalidate(s->a);
        }
        if (!r) {
            r = st_invalidate(s->b);
        }
    }
    if (buf) {
        free(buf);
    }
    if (!r) {
        u8 h[8] = { 'C', '2', 'E', 'E', ST_LAYOUT, 0 };
        u16 crc = crc16(0xFFFF, h, 6);
        h[6] = crc;
        h[7] = crc >> 8;
        r = eeprom24_write(&c2_eeprom, ST_HDR, h, sizeof(h));
    }
    gp_log("store: EEPROM formatted (layout %u): %s; factory cal:%s%s%s; from syscfg:%s",
           ST_LAYOUT, r ? "FAILED" : "ok", mask & 1 ? " sticks+triggers" : "", mask & 2 ? " imu" : "",
           mask ? "" : " none", moved[0] ? moved : " nothing");
    return r;
}

static void st_init(void)
{
    if (st_state) {
        return;
    }
    st_state = 2;
    if (jl_iic_init()) {
        gp_log("store: IIC init failed: syscfg");
        return;
    }
    jl_iic_task_busy = 1;
    int here = jl_iic_probe(c2_eeprom.addr);
    jl_iic_task_busy = 0;
    if (!here) {
        gp_log("store: no EEPROM at %02X: syscfg", c2_eeprom.addr);
        return;
    }
    u8 h[8];
    if (eeprom24_read(&c2_eeprom, ST_HDR, h, sizeof(h))) {
        gp_log("store: EEPROM read failed: syscfg");
        return;
    }
    int ok = !memcmp(h, "C2EE", 4) && h[4] == ST_LAYOUT && crc16(0xFFFF, h, 6) == (h[6] | h[7] << 8);
    if (!ok && st_format()) {
        return;                                          /* syscfg this boot; formats again next boot */
    }
    st_state = 1;
}

int gp_store_read(u8 id, void *buf, int len)
{
    st_init();
    const struct st_slot *s = st_find(id);
    if (st_state != 1 || !s) {
        return syscfg_read(id, buf, len);
    }
    u16 seq, n;
    u16 addr = st_newest(s, &seq, &n);
    if (!addr) {
        return -1;
    }
    n = MIN(n, len);
    return eeprom24_read(&c2_eeprom, addr + ST_REC_HDR, buf, n) ? -1 : n;
}

int gp_store_write(u8 id, const void *buf, int len)
{
    st_init();
    const struct st_slot *s = st_find(id);
    if (st_state != 1 || !s) {
        return syscfg_write(id, (void *)buf, len);
    }
    if (len > s->size - ST_REC_HDR) {
        gp_log("store: record %u too big (%d > %d)", id, len, s->size - ST_REC_HDR);
        return -1;
    }
    u16 seq = 0, n;
    u16 cur = st_newest(s, &seq, &n);
    u16 to = cur == s->a ? s->b : s->a;
    int r = st_write_copy(id, to, seq + 1, buf, len);
    if (r) {
        gp_log("store: write of record %u failed (%d)", id, r);
        return -1;
    }
    return len;
}

/* For the log: the factory record ([mask][66][14]) into buf; its length, or < 0. */
int c2_store_factory(u8 *buf, int len)
{
    st_init();
    u16 seq, n;
    if (st_state != 1 || !st_copy_valid(ST_ID_FACTORY, ST_FACTORY, ST_FACTORY_SLOT - ST_FACTORY, &seq, &n)) {
        return -1;
    }
    n = MIN(n, len);
    return eeprom24_read(&c2_eeprom, ST_FACTORY + ST_REC_HDR, buf, n) ? -1 : n;
}

#endif
