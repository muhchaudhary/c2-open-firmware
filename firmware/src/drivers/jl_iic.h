/*
 * jl_iic.h: register access on the BR23's hardware IIC (hw_iic_cfg[0], set
 * by the board), with every wait bounded. The SDK's hw_iic_tx_byte /
 * hw_iic_rx_byte spin on the pending flag forever, and sensors are read from
 * the report tick's timer interrupt: one stuck transfer would hang it until
 * the watchdog reset the chip. A wait that runs out counts in gp_hang_count.
 */
#ifndef JL_IIC_H
#define JL_IIC_H

#include "typedef.h"

int jl_iic_init(void);                                  /* 0 = ok */
int jl_iic_write(u8 addr, u8 reg, u8 val);              /* 1 = acked */
int jl_iic_read(u8 addr, u8 reg, u8 *buf, int len);     /* 1 = ok */
/* The same with a 2-byte register address, high byte first (24C32..24C512
 * EEPROMs): the address write only sets the read pointer. */
int jl_iic_read16(u8 addr, u16 reg, u8 *buf, int len);  /* 1 = ok */
/* Write len bytes after a 2-byte register address (an EEPROM page write:
 * the caller keeps it inside one page). 1 = every byte acked. */
int jl_iic_write16(u8 addr, u16 reg, const u8 *buf, int len);
/* 1 if a device acks addr (8-bit form, write bit): start, the address byte,
 * stop. No data byte, so nothing is written (i2cdetect's quick write). */
int jl_iic_probe(u8 addr);

/* Sharing the bus between the report tick (a timer interrupt: the IMU) and
 * task code (the EEPROM driver). Task code sets jl_iic_task_busy around each
 * transaction; the tick skips the bus while it is set (one IMU read missed,
 * the last sample kept). One core and the tick runs to completion, so when
 * task code runs no tick transfer is half done, and a flag is enough. */
extern volatile u8 jl_iic_task_busy;

#endif
