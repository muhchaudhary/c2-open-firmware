/*
 * eeprom24.h: a 24Cxx I2C EEPROM with 2-byte addresses (the C2's 24C64-class
 * chip at 0xA0, 8 KB, 32-byte pages; docs/stock-features.md), on the shared
 * hardware IIC (jl_iic.h).
 *
 * Task context only (the app task, boot): every call blocks, a write ~10 ms
 * per page changed (os_time_dly while the chip programs). Each bus
 * transaction holds jl_iic_task_busy, so the report tick skips the IMU for
 * that tick instead of breaking into it. Not for the tick itself.
 *
 * Writes go page by page; a page whose bytes are already there is not
 * written (no wear), and every written page is read back and compared.
 */
#ifndef EEPROM24_H
#define EEPROM24_H

#include "typedef.h"

struct eeprom24 {
    u8 addr;                /* 8-bit I2C address (0xA0) */
    u16 size;               /* bytes (0x2000) */
    u8 page;                /* page size, a power of two (32) */
};

/* Results: 0 ok; < 0 an error. */
#define EE24_OK          0
#define EE24_ERR_RANGE  -1  /* outside 0..size */
#define EE24_ERR_BUS    -2  /* not acked / a transfer timed out */
#define EE24_ERR_BUSY   -3  /* the chip didn't finish programming in time */
#define EE24_ERR_VERIFY -4  /* read back differs from what was written */

int eeprom24_read(const struct eeprom24 *ee, u16 at, u8 *buf, u16 len);
int eeprom24_write(const struct eeprom24 *ee, u16 at, const u8 *buf, u16 len);

/* Statistics since boot: pages written, pages skipped (already equal). */
extern u32 eeprom24_pages_written, eeprom24_pages_skipped;

#endif
