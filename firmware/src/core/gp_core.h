/*
 * gp_core.h: the report tick and the app-task poll, tying the board, the
 * protocol and the transport together.
 */
#ifndef GP_CORE_H
#define GP_CORE_H

void gp_core_start(void);               /* once BT is up: drivers, tick, poll */
void gp_core_tick_stop(void);           /* power-off: no more reports or sensor work */
void gp_core_restart(const char *why);  /* poll context: close the link, then restart (a normal boot) */
void gp_core_link_hello(void);          /* any context: send a link hello (gp_link.h) on the next tick */
void gp_core_link_unsent(u8 id);        /* gp_bt.c: a link hello / status didn't go out: again */
void gp_core_lowbat_test(void);        /* any context: show the low-battery warning for 10 s */

#endif
