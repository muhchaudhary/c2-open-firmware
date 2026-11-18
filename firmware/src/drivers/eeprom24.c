/* eeprom24.c: see eeprom24.h. */
#include "system/includes.h"
#include "jl_iic.h"
#include "eeprom24.h"

#define EE24_CHUNK          32      /* bytes per read transaction (~1 ms at 400 kHz) */
#define EE24_READY_TRIES    4       /* x 10 ms (one OS tick); the 24C64's tWR is 5 ms max */

u32 eeprom24_pages_written, eeprom24_pages_skipped;

static int ee_read_chunk(const struct eeprom24 *ee, u16 at, u8 *buf, u16 n)
{
    jl_iic_task_busy = 1;
    int ok = jl_iic_read16(ee->addr, at, buf, n);
    jl_iic_task_busy = 0;
    return ok;
}

int eeprom24_read(const struct eeprom24 *ee, u16 at, u8 *buf, u16 len)
{
    if ((u32)at + len > ee->size) {
        return EE24_ERR_RANGE;
    }
    while (len) {
        u16 n = len < EE24_CHUNK ? len : EE24_CHUNK;
        if (!ee_read_chunk(ee, at, buf, n)) {
            return EE24_ERR_BUS;
        }
        at += n;
        buf += n;
        len -= n;
    }
    return EE24_OK;
}

/* While the chip programs a page it acks nothing: wait until it acks its
 * address again (acknowledge polling). */
static int ee_wait_ready(const struct eeprom24 *ee)
{
    for (int i = 0; i < EE24_READY_TRIES; i++) {
        os_time_dly(1);
        jl_iic_task_busy = 1;
        int ok = jl_iic_probe(ee->addr);
        jl_iic_task_busy = 0;
        if (ok) {
            return 1;
        }
    }
    return 0;
}

int eeprom24_write(const struct eeprom24 *ee, u16 at, const u8 *buf, u16 len)
{
    if ((u32)at + len > ee->size) {
        return EE24_ERR_RANGE;
    }
    u8 cur[EE24_CHUNK];
    while (len) {
        /* up to the end of this page: the chip wraps inside a page */
        u16 room = ee->page - (at & (ee->page - 1));
        u16 n = len < room ? len : room;
        if (n > sizeof(cur)) {
            n = sizeof(cur);
        }
        if (!ee_read_chunk(ee, at, cur, n)) {
            return EE24_ERR_BUS;
        }
        if (memcmp(cur, buf, n)) {
            jl_iic_task_busy = 1;
            int ok = jl_iic_write16(ee->addr, at, buf, n);
            jl_iic_task_busy = 0;
            if (!ok) {
                return EE24_ERR_BUS;
            }
            if (!ee_wait_ready(ee)) {
                return EE24_ERR_BUSY;
            }
            if (!ee_read_chunk(ee, at, cur, n)) {
                return EE24_ERR_BUS;
            }
            if (memcmp(cur, buf, n)) {
                return EE24_ERR_VERIFY;
            }
            eeprom24_pages_written++;
        } else {
            eeprom24_pages_skipped++;
        }
        at += n;
        buf += n;
        len -= n;
    }
    return EE24_OK;
}
