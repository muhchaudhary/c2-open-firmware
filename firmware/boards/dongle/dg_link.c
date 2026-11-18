/* dg_link.c: the pad's receiver-link messages (core/gp_link.h) in, SInput
 * reports out on USB (dg_usb.c). From the BT task (dg_bt_hid_in).
 *
 *  - STATE: decoded and encoded as SInput's state report (the encoder the
 *    pad uses on USB, core/gp_sinput_desc.c), with the last STATUS's
 *    battery and a microsecond timestamp rebuilt from the pad's ms clock.
 *    The sequence numbers count lost reports.
 *  - HELLO: kept; SInput's features request is answered from it here (no
 *    round trip to the pad). Until one is in, the receiver asks for it.
 *  - The hello's mode picks what the receiver is on USB (dg_usb.c): Steam
 *    SInput, Xbox the wired Xbox 360 pad (the pad's xinput_encode_state),
 *    DS4 the DualShock 4 (the pad's ds4_encode_usb, battery in the state).
 *  - Ping (`03 8E` from the host, tools/c2lat.py): ask the pad for a hello,
 *    answer `02 8E` when it comes: a round trip over the air, timed in
 *    stages (dg_lat.echo, as the features round trip was before).
 * A pad without the link (SInput reports) still goes straight through
 * (dg_usb_forward). */
#include "system/includes.h"
#include "app_config.h"

#ifdef CONFIG_DONGLE_FW

#include <string.h>
#include "dongle.h"
#include "gp_log.h"
#include "gp_link.h"
#include "gp_sinput.h"
#include "gp_xinput.h"
#include "gp_ds4.h"

extern unsigned long jiffies_msec(void);

static struct gp_link_hello dg_hello;
static volatile u8 dg_have_hello;
static struct gp_battery dg_batt = { .plug = 4, .percent = 100 };
static u8 dg_seq_next, dg_have_seq;
static u32 dg_ms_total, dg_ms_last;          /* the pad's ms clock, unwrapped */
static u32 dg_hello_asked_ms;
volatile u32 dg_link_states, dg_link_lost;

void dg_link_reset(void)                    /* the link went down */
{
    dg_have_hello = 0;
    sinput_battery_reset();
    dg_have_seq = 0;
    dg_ms_total = 0;
    dg_echo_state = 0;
}

int dg_link_active(void)
{
    return dg_have_hello;
}

static void dg_link_ask_hello(void)
{
    static const u8 zero;
    dg_hello_asked_ms = jiffies_msec();
    dg_bt_output(GP_LINK_ID_HELLO_REQ, &zero, 1);
}

static void dg_link_state(const u8 *p, int len)
{
    struct gp_state s;
    u8 seq;
    u16 ms;
    if (!gp_link_decode_state(p, len, &seq, &s, &ms)) {
        return;
    }
    dg_link_states++;
    if (dg_have_seq && seq != dg_seq_next) {
        dg_link_lost += (u8)(seq - dg_seq_next);
    }
    dg_seq_next = seq + 1;
    if (!dg_have_seq) {
        dg_ms_last = ms;
    }
    dg_have_seq = 1;
    dg_ms_total += (u16)(ms - dg_ms_last);
    dg_ms_last = ms;
    if (dg_usb_xinput()) {
        u8 x[XINPUT_REPORT_LEN];
        xinput_encode_state(x, &s);                  /* the pad's own wired-Xbox encoder */
        dg_usb_put(DG_ID_XINPUT, x, sizeof(x));
    } else if (dg_usb_ds4()) {
        static u8 counter;
        u8 rep[DS4_USB_REPORT_LEN];
        ds4_encode_usb(rep, &s, &dg_batt, dg_ms_total * 1000, counter++);   /* the pad's own wired-DS4 encoder */
        dg_usb_put(DS4_ID_USB_STATE, rep, sizeof(rep));
    } else {
        u8 rep[SINPUT_REPORT_LEN];
        sinput_encode_state(rep, &s, &dg_batt, dg_ms_total * 1000);
        dg_usb_put(SINPUT_ID_STATE, rep, sizeof(rep));
    }
    if (!dg_have_hello && jiffies_msec() - dg_hello_asked_ms > 200) {
        dg_link_ask_hello();                         /* no hello yet (or it was lost) */
    }
}

static void dg_link_hello(const u8 *p, int len)
{
    struct gp_link_hello h;
    if (!gp_link_decode_hello(p, len, &h)) {
        return;
    }
    if (!dg_have_hello || h.mode != dg_hello.mode) {
        gp_log("link: hello v%u, mode %s, serial %02X%02X%02X%02X%02X%02X, tick %u us", h.version,
               h.mode == GP_MODE_XBOX ? "Xbox" : h.mode == GP_MODE_DS4 ? "DS4" : "Steam", h.serial[0], h.serial[1], h.serial[2], h.serial[3],
               h.serial[4], h.serial[5], h.tick_us);
    }
    dg_hello = h;
    dg_have_hello = 1;
    dg_usb_set_mode(h.mode);                         /* Steam: SInput on USB, Xbox: XInput */
    if (dg_echo_state == 2) {                       /* a ping's answer */
        u8 rep[SINPUT_REPORT_LEN];
        memset(rep, 0, sizeof(rep));
        rep[0] = DG_PING_CMD;
        dg_echo_t2 = dg_us();
        dg_echo_state = 3;
        dg_usb_put(SINPUT_ID_FEATURES, rep, sizeof(rep));
    }
}

void dg_link_in(u8 id, const u8 *p, int len)
{
    switch (id) {
    case GP_LINK_ID_STATE:
        dg_link_state(p, len);
        break;
    case GP_LINK_ID_STATUS:
        gp_link_decode_status(p, len, &dg_batt);
        /* Only once up as the SInput gamepad: a report put before that is
         * dropped, and the "due" spent on it waited out the minute
         * (2026-11-16: Linux's battery read "No data" ~1 min after linking). */
        if (dg_usb_sinput_up() && sinput_battery_due(&dg_batt, jiffies_msec())) {
            u8 rep[SINPUT_REPORT_LEN];                   /* Linux's battery (gp_sinput_desc.c): on a change, once a minute */
            sinput_encode_battery(rep, &dg_batt);
            dg_usb_put(SINPUT_ID_BATTERY, rep, sizeof(rep));
        }
        break;
    case GP_LINK_ID_HELLO:
        dg_link_hello(p, len);
        break;
    default:
        break;
    }
}

/* SInput's features request (03 02), from the hello. 0 if there's none
 * (an SInput pad: the request goes to it). Any context. */
int dg_link_features(void)
{
    if (!dg_have_hello) {
        return 0;
    }
    u8 rep[SINPUT_REPORT_LEN];
    sinput_encode_features(rep, &dg_hello.caps, dg_hello.serial);
    rep[7] = (u8)dg_hello.tick_us;                   /* polling interval: the pad's (docs/sinput.md 2.4) */
    rep[8] = (u8)(dg_hello.tick_us >> 8);
    dg_usb_queue(SINPUT_ID_FEATURES, rep, sizeof(rep));   /* we're in the USB interrupt's SET_REPORT */
    return 1;
}

const u8 *dg_link_serial(void)
{
    static const u8 none[6];
    return dg_have_hello ? dg_hello.serial : none;
}

/* The host's ping (03 8E): any context (the USB interrupt). */
void dg_link_ping(void)
{
    dg_echo_t0 = dg_us();
    dg_echo_state = 1;
    dg_link_ask_hello();
}

#endif
