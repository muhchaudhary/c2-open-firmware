/* Host build: the SDK's app_var, as battery_adc.c sets it. */
#ifndef APP_MAIN_H
#define APP_MAIN_H
#include "typedef.h"
struct app_var_t {
    u16 warning_tone_v;
    u16 poweroff_tone_v;
};
extern struct app_var_t app_var;
#endif
