/* SPDX-License-Identifier: GPL-2.0-only
 * temp_sense.c — NTC thermistor temperature measurement
 *
 * Circuit: VCC_3V3 → R1(10kΩ) → node → NTC(B3950 10kΩ) → GND
 * ADC:     PA0 / ADC1 CH0, 12-bit, software trigger
 *
 * Steinhart-Hart coefficients (B3950 10 kΩ):
 *   A = 0.001129148
 *   B = 0.000234125
 *   C = 8.76741e-8
 *
 * 1/T_K = A + B·ln(R) + C·ln(R)³
 */

#include "temp_sense.h"
#include "main.h"
#include <math.h>

#define R_FIXED_OHMS    10000.0f
#define ADC_FULL_SCALE  4095.0f
#define SH_A            0.001129148f
#define SH_B            0.000234125f
#define SH_C            8.76741e-8f

static int16_t s_temp_filtered_c10 = 250;  /* 25.0 °C initial */

/* ─────────────────────────────────────────────────────────────── */
int16_t temp_sense_update(void)
{
    /* Select ADC channel (assume CubeMX configures scan or we set rank) */
    ADC_ChannelConfTypeDef ch = {
        .Channel      = TEMP_ADC_CHANNEL,
        .Rank         = ADC_REGULAR_RANK_1,
        .SamplingTime = ADC_SAMPLINGTIME_COMMON_1,
    };
    HAL_ADC_ConfigChannel(&hadc1, &ch);
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 10);
    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    if (raw == 0 || raw >= ADC_FULL_SCALE) {
        /* Open/short circuit — return last value */
        return s_temp_filtered_c10;
    }

    /* R_ntc = R_fixed × raw / (4095 - raw)  [top-side voltage divider] */
    float r_ntc = R_FIXED_OHMS * (float)raw / (ADC_FULL_SCALE - (float)raw);

    /* Steinhart-Hart */
    float ln_r = logf(r_ntc);
    float inv_t = SH_A + SH_B * ln_r + SH_C * (ln_r * ln_r * ln_r);
    float t_celsius = (1.0f / inv_t) - 273.15f;

    /* Convert to ×10 and clamp to sensor range */
    int16_t t_c10 = (int16_t)(t_celsius * 10.0f);
    if (t_c10 < -500) t_c10 = -500;
    if (t_c10 >  800) t_c10 =  800;

    /* IIR filter: new = 0.9 × old + 0.1 × sample */
    s_temp_filtered_c10 = (int16_t)(
        (TEMP_IIR_ALPHA_X10 * (int32_t)s_temp_filtered_c10
         + (10 - TEMP_IIR_ALPHA_X10) * (int32_t)t_c10) / 10);

    return s_temp_filtered_c10;
}

int16_t temp_sense_get(void)
{
    return s_temp_filtered_c10;
}
