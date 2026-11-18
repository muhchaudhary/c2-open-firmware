/*
 * ws2812_spi.h: a chain of WS2812-style RGB LEDs on SPI1's DO, one SPI byte
 * per LED bit, sent like the SDK's led_spi_send_rgbbuf (cpu/br23/led_spi.c,
 * which also sets up the pin: led_spi_init()).
 *
 * Callable from any task: one sender at a time, a second caller drops its
 * frame (the next update resends the full state). Every wait on the SPI is
 * bounded (gp_hang_count).
 */
#ifndef WS2812_SPI_H
#define WS2812_SPI_H

#include "typedef.h"

#define WS2812_MAX          8

void ws2812_spi_send(const u8 (*rgb)[3], int n);

#endif
