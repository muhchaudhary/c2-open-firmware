/* gp_store.c: see gp_store.h. The default medium, syscfg; weak, so a board
 * with its own storage replaces both. */
#include "system/includes.h"
#include "syscfg_id.h"
#include "gp_store.h"

__attribute__((weak)) int gp_store_read(u8 id, void *buf, int len)
{
    return syscfg_read(id, buf, len);
}

__attribute__((weak)) int gp_store_write(u8 id, const void *buf, int len)
{
    return syscfg_write(id, (void *)buf, len);
}
