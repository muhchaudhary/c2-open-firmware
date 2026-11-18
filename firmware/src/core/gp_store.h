/*
 * gp_store.h: records kept across power-offs (the settings, the
 * calibration), by id. Same contract as the SDK's syscfg_read/write: a read
 * returns the bytes read (a record shorter than buf leaves the rest alone),
 * < 0 if there is none; a write returns len on success.
 *
 * The medium is the board's: by default the SDK's syscfg (the VM in flash,
 * item = id; gp_store.c), which a board overrides by defining gp_store_read
 * and gp_store_write itself (the C2: its I2C EEPROM, boards/c2/c2_store.c).
 *
 * Task context only (app task, boot): never the tick. A write may block for
 * tens of ms.
 */
#ifndef GP_STORE_H
#define GP_STORE_H

#include "typedef.h"

#define GP_STORE_CAL        48      /* gp_cal.c (syscfg user ids end at 49; apps/hid uses up to 47) */
#define GP_STORE_SETTINGS   49      /* gp_settings.c */

int gp_store_read(u8 id, void *buf, int len);
int gp_store_write(u8 id, const void *buf, int len);

#endif
