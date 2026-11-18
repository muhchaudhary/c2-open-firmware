/* jl_iic.c: see jl_iic.h. Same register sequence as cpu/br23/iic_hw.c, plus
 * a bound (a byte at 400 kHz is ~25 us; the bound is generous). */
#include "system/includes.h"
#include "asm/iic_hw.h"
#include "gp_log.h"
#include "jl_iic.h"

#define JL_IIC_SPIN         20000

volatile u8 jl_iic_task_busy;

static int jl_iic_wait(void)
{
    for (u32 n = 0; n < JL_IIC_SPIN; n++) {
        if (iic_pnd(JL_IIC)) {
            iic_pnd_clr(JL_IIC);
            return 1;
        }
    }
    gp_hang_count++;
    return 0;
}

static int jl_iic_tx(u8 byte)
{
    iic_dir_out(JL_IIC);
    iic_buf_reg(JL_IIC) = byte;
    iic_cfg_done(JL_IIC);
    return jl_iic_wait() && iic_send_is_ack(JL_IIC);
}

static int jl_iic_rx(u8 *byte, u8 ack)
{
    iic_dir_in(JL_IIC);
    if (ack) {
        iic_recv_ack(JL_IIC);
    } else {
        iic_recv_nack(JL_IIC);
    }
    iic_buf_reg(JL_IIC) = 0xff;
    iic_cfg_done(JL_IIC);
    if (!jl_iic_wait()) {
        return 0;
    }
    *byte = iic_buf_reg(JL_IIC);
    return 1;
}

int jl_iic_init(void)
{
    return hw_iic_init(0);
}

int jl_iic_write(u8 addr, u8 reg, u8 val)
{
    hw_iic_start(0);
    int ok = jl_iic_tx(addr) && jl_iic_tx(reg) && jl_iic_tx(val);
    hw_iic_stop(0);
    return ok;
}

int jl_iic_write16(u8 addr, u16 reg, const u8 *buf, int len)
{
    hw_iic_start(0);
    int ok = jl_iic_tx(addr & 0xfe) && jl_iic_tx(reg >> 8) && jl_iic_tx(reg & 0xff);
    for (int i = 0; ok && i < len; i++) {
        ok = jl_iic_tx(buf[i]);
    }
    hw_iic_stop(0);
    return ok;
}

int jl_iic_probe(u8 addr)
{
    hw_iic_start(0);
    int ok = jl_iic_tx(addr & 0xfe);
    hw_iic_stop(0);
    return ok;
}

int jl_iic_read16(u8 addr, u16 reg, u8 *buf, int len)
{
    hw_iic_start(0);
    int ok = jl_iic_tx(addr) && jl_iic_tx(reg >> 8) && jl_iic_tx(reg & 0xff);
    if (ok) {
        hw_iic_start(0);                                 /* repeated start */
        ok = jl_iic_tx(addr | 1);
        for (int i = 0; ok && i < len; i++) {
            ok = jl_iic_rx(&buf[i], i < len - 1);
        }
    }
    hw_iic_stop(0);
    return ok;
}

int jl_iic_read(u8 addr, u8 reg, u8 *buf, int len)
{
    hw_iic_start(0);
    int ok = jl_iic_tx(addr) && jl_iic_tx(reg);
    if (ok) {
        hw_iic_start(0);                                 /* repeated start */
        ok = jl_iic_tx(addr | 1);
        for (int i = 0; ok && i < len; i++) {
            ok = jl_iic_rx(&buf[i], i < len - 1);
        }
    }
    hw_iic_stop(0);
    return ok;
}
