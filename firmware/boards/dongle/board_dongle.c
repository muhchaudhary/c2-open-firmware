/* board_dongle.c: the C2's USB receiver (AC695X, 512 KB flash) as an SDK
 * board (docs/dongle.md): power, the ways back to the bootloader (button
 * held at power-up, the crash-loop guard), the LED (PA09; PA06 held high,
 * as stock) and the button (PA00 to ground). PA12 is USB D-: never touched.
 *
 * Power: LDO (works with or without a DC-DC inductor), VDDIO 3.2 V (the
 * pad's on-USB value), 24 MHz crystal (the stock receiver's).
 *
 * The crash-loop guard runs first in setup_arch() (patched in), before the
 * watchdog, clock and voltage set-up: three boots in a row that don't reach DG_STABLE_MS of uptime enter
 * the USB bootloader. Its counter is in .dg_noinit, a NOLOAD section that
 * startup does not clear (patch_sdk.py; the SDK's .non_volatile_ram is
 * zeroed with .bss). A magic word marks it valid, so a power cycle starts it
 * over. If the RAM doesn't survive a reset, the guard simply never fires. */
#include "app_config.h"

#ifdef CONFIG_BOARD_DONGLE

#include "system/includes.h"
#include "asm/power/p33.h"
#include "asm/power/power_port.h"
#include "usb/otg.h"
#include "dongle.h"

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[BOARD]"
#define LOG_ERROR_ENABLE
#define LOG_INFO_ENABLE
#include "debug.h"

/* ---- Power ----------------------------------------------------------------- */
struct low_power_param power_param = {
    .config         = TCFG_LOWPOWER_LOWPOWER_SEL,
    .btosc_hz       = TCFG_CLOCK_OSC_HZ,
    .delay_us       = TCFG_CLOCK_SYS_HZ / 1000000L,
    .btosc_disable  = TCFG_LOWPOWER_BTOSC_DISABLE,
    .vddiom_lev     = TCFG_LOWPOWER_VDDIOM_LEVEL,
    .vddiow_lev     = TCFG_LOWPOWER_VDDIOW_LEVEL,
    .osc_type       = OSC_TYPE_LRC,
    .dcdc_port      = TCFG_DCDC_PORT_SEL,
};

/* ---- USB: the SDK's OTG detection brings the device up ---------------------- */
#if TCFG_OTG_MODE
struct otg_dev_data otg_data = {
    .usb_dev_en = TCFG_OTG_USB_DEV_EN,
    .slave_online_cnt = TCFG_OTG_SLAVE_ONLINE_CNT,
    .slave_offline_cnt = TCFG_OTG_SLAVE_OFFLINE_CNT,
    .host_online_cnt = TCFG_OTG_HOST_ONLINE_CNT,
    .host_offline_cnt = TCFG_OTG_HOST_OFFLINE_CNT,
    .detect_mode = TCFG_OTG_MODE,
    .detect_time_interval = TCFG_OTG_DET_INTERVAL,
};
#endif

REGISTER_DEVICES(device_table) = {
#if TCFG_OTG_MODE
    { "otg",     &usb_dev_ops, (void *) &otg_data},
#endif
};

void debug_uart_init(const struct uart_platform_data *data)
{
}

u8 get_power_on_status(void)
{
    return 0;
}

/* ---- Back to the USB bootloader ---------------------------------------------
 * As the pad (board_c2.c) and stock / the SDK's go_mask_usb_updata(): boot
 * state UPGRADE_USB_SOFT_KEY (2), then a soft reset; the ROM comes up as the
 * JieLi USB bootloader (4c4a:2342). From RAM: the flash mapping goes off. */
extern void nvram_set_boot_state(u32 state);
extern void hw_mmu_disable(void);
extern void ram_protect_close(void);

AT(.volatile_ram_code)
static void dg_enter_isp(void)
{
    local_irq_disable();
    ram_protect_close();
    hw_mmu_disable();
    nvram_set_boot_state(2);
    JL_CLOCK->PWR_CON |= BIT(4);
    while (1);
}

void gp_board_enter_bootloader(void)        /* core/gp_log.c's "boot" command */
{
    dg_enter_isp();
}

void dg_enter_bootloader(void)
{
    dg_enter_isp();
}

/* ---- Checkpoints and the LED ---------------------------------------------------
 * A checkpoint is a breadcrumb only (dg_crumb; read with `jlflash.sh
 * crumbs`): 1 guard, 2 after the clock set-up, 3 board_init, 4 after power
 * init, 5 after devices_init, 6 app started. The bring-up blinks and the
 * D+ / D- pulses are gone (the pulses showed on the PC as `error -32`
 * connects, the delays made the boot ~27 s).
 * The LED is PA06 or PA09 (stock drives both as outputs: PA06 high, PA09
 * cleared before its bootloader entry). The heartbeat blinks PA09 and holds
 * PA06 high as stock does: if the LED blinks, it is PA09. */
void dg_crumb(int n);
#define DG_LED_PINS         (BIT(6) | BIT(9))
#define DG_LED_PIN          BIT(9)
#define DG_LED_HOLD         BIT(6)

void dg_led(int on)
{
    JL_PORTA->DIR &= ~DG_LED_PINS;
    JL_PORTA->OUT |= DG_LED_HOLD;
    if (on) {
        JL_PORTA->OUT |= DG_LED_PIN;
    } else {
        JL_PORTA->OUT &= ~DG_LED_PIN;
    }
}

void dg_checkpoint(int n)
{
    dg_crumb(n);
}

/* ---- USB pins ---------------------------------------------------------------------
 * Before USB starts: both as inputs with no pulls, then back to the USB
 * block, whatever the loader left. (Added when the PC saw a low-speed
 * device; that turned out to be a pull-up on PA12, which is wired to D-.
 * This reset is kept as a sane starting state.) */
static void dg_usb_pins_reset(void)
{
    usb_iomode(1);
    gpio_set_pull_up(IO_PORT_DP, 0);
    gpio_set_pull_down(IO_PORT_DP, 0);
    gpio_set_pull_up(IO_PORT_DM, 0);
    gpio_set_pull_down(IO_PORT_DM, 0);
    gpio_set_direction(IO_PORT_DP, 1);
    gpio_set_direction(IO_PORT_DM, 1);
    usb_iomode(0);
}

/* ---- Crash-loop guard -------------------------------------------------------- */
#define DG_GUARD_MAGIC      0x6447A5C3u
#define DG_GUARD_LIMIT      3

/* Breadcrumbs, in RAM that startup doesn't clear (.dg_noinit, at 0x9A24):
 * read back from the bootloader with `jlflash.sh crumbs`. last[] holds the
 * last checkpoint each of the last 16 boots reached (index boots % 16):
 * 1..6 as the LED checkpoints, 7 USB host seen, 8 stable (10 s), 0xEE the
 * guard entering the bootloader. */
struct dg_noinit {
    u32 magic;                              /* DG_GUARD_MAGIC: the block is valid */
    u32 run;                                /* boots in a row without DG_STABLE_MS (the guard) */
    u32 boots;                              /* boots since the block was (re)initialised */
    u32 rst_src;                            /* P3_RST_SRC of the latest boot */
    u8 last[16];
    u8 early[8];                            /* marks before main(): 0xA5 = reached (dg_early_mark) */
};
static volatile struct dg_noinit dg_nv __attribute__((section(".dg_noinit")));
u8 dg_boot_count;                           /* this boot's number in the run (for the log) */
u8 dg_boot_rst_src;                         /* why the chip started (P3_RST_SRC) */

/* Marks before the guard, from code the prebuilt startup calls: 0
 * app_load_common_code() (after .bss / .data / cache set-up), 1 / 2
 * update_result_get() start / end, 3 the guard. RAM is random until the
 * first mark, so mark 0 clears the others: only 0xA5 counts. */
void dg_early_mark(int k)
{
    if (k == 0) {
        for (int i = 0; i < 8; i++) {
            dg_nv.early[i] = 0;
        }
    }
    dg_nv.early[k] = 0xA5;
}

void dg_crumb(int n)
{
    dg_nv.last[dg_nv.boots & 15] = n;
}

/* The receiver's button: PA00, to ground (the pull-up holds it high;
 * pressed reads 0). Held at power-up -> the USB bootloader, checked first
 * thing (dg_guard_boot), so no later code can stand in the way. Debounced:
 * all of 20 samples over a few ms must read low. */
#define DG_BUTTON           BIT(0)

static int dg_button_held(void)
{
    JL_PORTA->DIR |= DG_BUTTON;
    JL_PORTA->PD &= ~DG_BUTTON;
    JL_PORTA->PU |= DG_BUTTON;
    JL_PORTA->DIE |= DG_BUTTON;
    for (volatile int i = 0; i < 2000; i++);         /* let the pull-up settle */
    for (int n = 0; n < 20; n++) {
        if (JL_PORTA->IN & DG_BUTTON) {
            return 0;
        }
        for (volatile int i = 0; i < 2000; i++);
    }
    return 1;
}

/* Called first in setup_arch() (patched in by firmware/sdk/patch_sdk.py),
 * before the watchdog, clocks and voltages are set up. */
void dg_guard_boot(void)
{
    dg_early_mark(3);
    if (dg_nv.magic != DG_GUARD_MAGIC) {     /* power-on: RAM is random */
        dg_nv.magic = DG_GUARD_MAGIC;
        dg_nv.run = 0;
        dg_nv.boots = 0;
        for (int i = 0; i < 16; i++) {
            dg_nv.last[i] = 0;
        }
    }
    dg_nv.boots++;
    dg_nv.rst_src = p33_rx_1byte(P3_RST_SRC);
    dg_checkpoint(1);
    if (dg_button_held()) {                  /* button held at power-up */
        dg_nv.run = 0;
        dg_crumb(0xB0);
        dg_enter_isp();
    }
    dg_nv.run++;
    dg_boot_count = dg_nv.run;
    if (dg_nv.run >= DG_GUARD_LIMIT) {
        dg_nv.run = 0;                       /* the next boot (after the bootloader) starts over */
        dg_crumb(0xEE);
        dg_enter_isp();
    }
}

void dg_guard_stable(void)                  /* DG_STABLE_MS of uptime: not a crash loop */
{
    dg_nv.run = 0;
    dg_crumb(8);
}

/* ---- The button, at run time (a short press pairs: dg_main.c) ----------------- */
void dg_button_init(void)
{
    JL_PORTA->DIR |= DG_BUTTON;
    JL_PORTA->PD &= ~DG_BUTTON;
    JL_PORTA->PU |= DG_BUTTON;
    JL_PORTA->DIE |= DG_BUTTON;
    /* PA12 is NOT touched: it is wired to USB D- (a pull-up on it made the
     * PC see a low-speed device that never answered, 2026-11-13). */
}

int dg_button_pressed(void)
{
    return !(JL_PORTA->IN & DG_BUTTON);
}

/* ---- Board init ---------------------------------------------------------------- */
static void sleep_exit_callback(u32 usec)
{
}

static void sleep_enter_callback(u8 step)
{
}

void board_set_soft_poweroff(void)
{
    /* The receiver never powers itself off (the app never asks). */
}

void board_power_init(void)
{
    power_init(&power_param);
    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);     /* isd_config RESET = PB01_00_0: off */
    gpio_shortpress_reset_config(0);
    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter_callback, sleep_exit_callback,
                       board_set_soft_poweroff);
    power_keep_dacvdd_en(0);
}

extern void cfg_file_parse(u8 idx);

void board_init()
{
    dg_boot_rst_src = p33_rx_1byte(P3_RST_SRC);
    dg_checkpoint(3);
    dg_usb_pins_reset();
    board_power_init();
    dg_checkpoint(4);
    adc_vbg_init();
    adc_init();
    cfg_file_parse(0);                       /* the BT configuration (name, address, ...), as the pad */
    devices_init();
    dg_checkpoint(5);
    power_set_mode(PWR_LDO15);
}

#endif
