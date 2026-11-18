/*
 * gamepad.h: the core's view of a gamepad. Every board fills a struct
 * gp_state each report tick; the core turns it into SInput reports and
 * sends them over the active transport (gp_transport.h).
 *
 * Contexts: "tick" is the report tick (GP_TICK_MS, the SDK's usr_timer, a
 * hardware timer interrupt); "poll" is the app task's housekeeping timer
 * (GP_POLL_MS). Anything that writes flash or talks to the BT stack's
 * command path runs in the poll, never in the tick.
 */
#ifndef GAMEPAD_H
#define GAMEPAD_H

#include "typedef.h"

/* The tick is the SDK's usr_timer. Its base is 2 ms (as stock's: TIMER1,
 * 24000 x 2 counts at 24 MHz) and a timer fires late: asking 6 ms gives
 * ~6.4 ms idle, ~7.7 ms connected (debug log, 2026-11-13; asking 8 gave
 * 10). The report timestamp follows the real clock, not the tick count. (A
 * tick on JL_TIMER3 hung the pad at boot; JL_TIMER PRD is 16 bits.)
 * Bluetooth (2026-11-14, tools/c2rate.py on one PC): at 8 ms the link
 * carried ~110 reports/s, p95 interval 27 ms; stock v19 on the same PC
 * ~300/s (its report timestamps step 2 ms: a 2 ms tick that skips while
 * the link is busy), p95 6-7 ms. Reports now go straight to the
 * controller (gp_bt.c) and the CPU runs at 96 MHz (it was the SDK HID
 * default 24 MHz; at 24 MHz a 2 ms tick starved the idle task and the
 * watchdog reset the pad). The tick asks for the 2 ms base, as stock;
 * GP_TICK_MS is the nominal rate told to hosts only: the tick fires about
 * every 4 ms (docs/firmware-design.md), and stick and trigger code runs only when a
 * report is built, so durations go by gp_ms(), never by counting ticks. */
#define GP_TICK_MS          3
#define GP_TICK_REQ_MS      2
#define GP_POLL_MS          50

/* The system's millisecond clock (2 ms steps, any context). Durations: keep
 * a start time and compare gp_ms() - start (wraps safely, u32). */
extern unsigned long jiffies_msec(void);
static inline u32 gp_ms(void)
{
    return jiffies_msec();
}

/* Microseconds, for measurements: the millisecond clock plus the system
 * timer's count within its 2 ms step. In an interrupt the millisecond part
 * may not have moved on yet when the count wraps: a span read there can be
 * 2000 us short (gp_us_span corrects a negative one). */
extern u32 local_timer_us_time(void);
static inline u32 gp_us(void)
{
    u32 j, l;
    do {
        j = jiffies_msec();
        l = local_timer_us_time();
    } while (j != jiffies_msec());
    return j * 1000 + l;
}
static inline u32 gp_us_span(u32 from, u32 to)
{
    s32 d = (s32)(to - from);
    return d < 0 ? d + 2000 : d;
}

/* Buttons. Bits 0..23 are SInput's button bytes 3..5 as sent
 * (docs/sinput.md 2.1); bits from 24 up are never reported. */
enum {
    GP_BTN_SOUTH = 0, GP_BTN_EAST, GP_BTN_WEST, GP_BTN_NORTH,
    GP_BTN_UP, GP_BTN_DOWN, GP_BTN_LEFT, GP_BTN_RIGHT,
    GP_BTN_L3 = 8, GP_BTN_R3, GP_BTN_L1, GP_BTN_R1,
    GP_BTN_L2, GP_BTN_R2, GP_BTN_PADDLE_L1, GP_BTN_PADDLE_R1,
    GP_BTN_START = 16, GP_BTN_BACK, GP_BTN_GUIDE, GP_BTN_CAPTURE,
    GP_BTN_MODIFIER = 31,       /* the pad's own function key (C2: "M"); hotkeys only */
};
#define GP_BIT(b)           (1u << (b))
#define GP_REPORTED_BUTTONS 0x00ffffffu

struct gp_state {
    u32 buttons;                /* GP_BIT(GP_BTN_*) */
    s16 lx, ly, rx, ry;         /* -32767..32767, +Y = down */
    s16 lt, rt;                 /* -32768 released .. 32767 full */
    s16 imu[6];                 /* SInput frame: accel X Y Z, gyro X Y Z */
};

/* Battery and charging, as the SInput report carries it. */
struct gp_battery {
    u16 mv;
    u8  percent;
    u8  plug;                   /* SInput: 2 charging, 3 full, 4 on battery */
    u8  charging;
};

/* SInput identity of the pad (features reply, docs/sinput.md 2.4). */
struct gp_sinput_caps {
    u8  features[2];            /* feature flags bytes 4, 5 */
    u8  gamepad_type;           /* SDL_GamepadType */
    u8  face_style;             /* face style << 4 | sub-product */
    u16 accel_g, gyro_dps;      /* full-scale ranges of gp_state.imu */
    u8  button_mask[4];
};

/* LED chain layout. Slot numbers are positions in the chain. */
#define GP_LEDS_MAX         8
struct gp_led_layout {
    u8 count;
    u8 home;                    /* the Home / status LED */
    u8 player[4];               /* player 1..4 indicators, top to bottom */
    u8 dock;                    /* the charging dock's LED (the chain runs on through the dock's
                                 * contacts), GP_LED_NONE if there is none */
};
#define GP_LED_NONE         0xff

/* Input modes (gp_settings.mode; Home + A / X / B / Y, as stock's combos): what the pad is
 * to a host. Steam: SInput over Bluetooth and USB. Xbox: an Xbox One S pad
 * over Bluetooth, an Xbox 360 pad over USB, as stock. DS4: a DualShock 4
 * over both (gp_ds4.h; for PCs, not a PS4 console). Switch: a Switch Pro
 * Controller over both (gp_switch.h; the console and PCs). */
enum gp_mode { GP_MODE_STEAM, GP_MODE_XBOX, GP_MODE_DS4, GP_MODE_SWITCH };
#define GP_MODE_LAST        GP_MODE_SWITCH
/* How the pad reaches the computer without a cable: Bluetooth to the host
 * itself, or the C2 receiver (its own Bluetooth identity and pairings,
 * gp_bt.c). Independent of the mode. */
enum gp_link { GP_LINK_BT, GP_LINK_RECEIVER };

/* Calibration (core/gp_cal.h). Values are in each driver's raw units:
 * sticks as the board feeds stick_axis (C2: 15-bit, rest ~16384), triggers
 * in ADC counts (falling as pressed), the gyro in SInput's frame. */
#define GP_CAL_STICKS       0x01
#define GP_CAL_TRIGGERS     0x02
#define GP_CAL_GYRO         0x04
struct gp_raw {
    u16 stick[4];               /* LX LY RX RY */
    u16 trig[2];                /* LT RT */
    u8  stick_ok;               /* bit per stick axis: a value has arrived */
};
struct gp_cal {
    u8  valid;                  /* GP_CAL_* parts stored */
    u16 stick_centre[4], stick_pos[4], stick_neg[4];   /* centre, travel up / down */
    u16 trig_rest[2], trig_span[2];
    s16 gyro_bias[3];
};

/* ---- What a board provides (boards/<name>/gamepad_<name>.c) ---------------- */
struct erm_motor;
struct gp_board_info {
    const char *name;                   /* Bluetooth name */
    struct gp_sinput_caps sinput;
    struct gp_led_layout leds;
    const struct erm_motor *motors;     /* [0] left (big), [1] right (small) */
};
extern const struct gp_board_info gp_board_info;

void gp_board_start(void);                      /* poll context, once BT is up: the input drivers */
u32  gp_board_buttons(void);                    /* any context: GP_BIT(GP_BTN_*) as held */
void gp_board_tick(void);                       /* tick, every one while a host is connected: sensor polling */
void gp_board_read(struct gp_state *s);         /* tick, for a state report: sticks, triggers, IMU (not buttons) */
void gp_board_battery(struct gp_battery *b);    /* tick or poll: updates the running average */
void gp_board_raw(struct gp_raw *r);            /* tick: the raw inputs the last gp_board_read used */
void gp_board_cal_apply(const struct gp_cal *c, u8 clear);  /* use the parts in c->valid; parts in clear learn again */
int  gp_board_usb_power(void);                  /* 5 V present on the USB socket */
void gp_board_dock_poll(void);                  /* every GP_POLL_MS, any mode (charging too): dock sense, dock power */
int  gp_board_docked(void);                     /* on a powered charging dock (debounced); -1: not known yet */
int  gp_board_charge_power(void);               /* any 5 V the charger can use: USB or the dock */
int  gp_board_charger_pin(void);                /* the charger status pin (C2: PB02, low = charging), -1 none */
void gp_board_led_power(int on);                /* the LED chain's supply */
void gp_board_led_send(const u8 (*rgb)[3], int n);   /* one frame on the LED chain */
void gp_board_power_down(void);                 /* before off: peripherals' power off */
int  gp_board_woken_by_usb_only(void);          /* this boot: woken from off by 5 V alone */
void gp_board_poll(void);                       /* poll context housekeeping */
int  gp_board_status(char *buf, int len);       /* debug log status line, the board's part */
void gp_board_enter_bootloader(void);           /* the chip's USB bootloader; doesn't return */

/* ---- Core services ------------------------------------------------------- */
extern volatile u8 gp_keep_awake;               /* 1: the SDK must not enter low-power sleep */

#endif
