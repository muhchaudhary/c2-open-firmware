/* Host build: the ADC, faked in fake.c (fake_adc[ch]). */
#ifndef ASM_ADC_API_H
#define ASM_ADC_API_H
#include "typedef.h"
u32 adc_get_value(u32 ch);              /* raw */
u32 adc_get_voltage(u32 ch);            /* mV at the pin */
void adc_add_sample_ch(u32 ch);
#endif
