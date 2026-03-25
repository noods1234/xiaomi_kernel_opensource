/* SPDX-License-Identifier: GPL-2.0-only
 * temp_sense.h — NTC thermistor temperature measurement
 *
 * Hardware: NTC 10 kΩ B3950 on PA0 / ADC1 CH0.
 * Divider (top-side): VCC_3V3 → R1(10kΩ) → NTC → GND.
 * Steinhart-Hart coefficients for B3950 10 kΩ NTC.
 */
#pragma once
#include <stdint.h>

/* ADC channel for NTC (PA0) */
#define TEMP_ADC_CHANNEL    ADC_CHANNEL_0

/* IIR filter coefficient: temp = α × temp + (1−α) × sample */
#define TEMP_IIR_ALPHA_X10  9   /* α = 0.9 (updated every 100 ms) */

/* Read one ADC sample and return temperature °C × 10.
 * Updates the internal IIR filter; call at ~100 ms intervals. */
int16_t temp_sense_update(void);

/* Return last filtered temperature (°C × 10). No ADC read. */
int16_t temp_sense_get(void);
