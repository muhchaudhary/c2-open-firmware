/*
 * gp_charging.h: charging mode: off with USB 5 V present (or woken from off
 * by 5 V alone), or put on the charging dock. No Bluetooth; the external
 * charger IC charges by itself and
 * the LEDs show stock's charging animation. Unplugged: off. Home pressed and
 * released: restart into normal use (stock "have charge sleep ps release
 * soft reset").
 */
#ifndef GP_CHARGING_H
#define GP_CHARGING_H

void gp_charging_start(void);

#endif
