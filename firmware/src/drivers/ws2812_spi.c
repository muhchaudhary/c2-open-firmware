/* ws2812_spi.c: see ws2812_spi.h.
 *
 * Bit patterns: the SDK's led_spi.c (8 MHz, 0x7C / 0x60, every bit starts
 * low) or stock GameSir's (0xFC / 0xC0, aimed at stock's ~6.7 MHz). Tested
 * on the C2 (2026-11-12): with stock's patterns the first LED shows every
 * frame one frame late; with the SDK's all of them are right. */
#include "system/includes.h"
#include "gp_log.h"
#include "ws2812_spi.h"

#define WS2812_STOCK_TIMING 0

static u8 ws_buf[WS2812_MAX * 24];
static volatile u8 ws_busy;

static void ws_spi_wait(void)
{
    for (u32 n = 0; n < 200000; n++) {
        if (JL_SPI1->CON & BIT(15)) {
            return;
        }
    }
    gp_hang_count++;
}

static void ws_send_frame(const u8 (*rgb)[3], int n)
{
    static u8 zero[16];
    u32 lsb = clk_get("lsb");
    u8 one = WS2812_STOCK_TIMING ? 0xFC : 0x7C, nil = WS2812_STOCK_TIMING ? 0xC0 : 0x60;
    for (int i = 0; i < n; i++) {
        u32 dat = (rgb[i][1] << 16) | (rgb[i][0] << 8) | rgb[i][2];   /* G R B */
        for (int k = 0; k < 24; k++) {
            ws_buf[i * 24 + k] = (dat & BIT(23 - k)) ? one : nil;
        }
    }
    JL_SPI1->BAUD = lsb / 1000000 - 1;          /* reset: 16 zero bytes at 1 MHz, as led_spi_rest() */
    JL_SPI1->CON |= BIT(14);
    JL_SPI1->ADR = (u32)zero;
    JL_SPI1->CNT = sizeof(zero);
    ws_spi_wait();
    JL_SPI1->CON |= BIT(14);
    JL_SPI1->BAUD = lsb / (WS2812_STOCK_TIMING ? 6700000 : 8000000) - 1;
    JL_SPI1->CON |= BIT(14);
    JL_SPI1->ADR = (u32)ws_buf;
    JL_SPI1->CNT = n * 24;
    ws_spi_wait();
    JL_SPI1->CON |= BIT(14);
}

void ws2812_spi_send(const u8 (*rgb)[3], int n)
{
    if (n > WS2812_MAX) {
        n = WS2812_MAX;
    }
    OS_ENTER_CRITICAL();
    u8 busy = ws_busy;
    ws_busy = 1;
    OS_EXIT_CRITICAL();
    if (busy) {
        return;
    }
    ws_send_frame(rgb, n);
    ws_busy = 0;
}
