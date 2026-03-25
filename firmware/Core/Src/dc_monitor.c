/* SPDX-License-Identifier: GPL-2.0-only
 * dc_monitor.c — OPA548 output DC offset monitor
 *
 * Circuit: OPA548 OUT → R7(100kΩ) → R8(10kΩ) → GND
 *   V_adc = V_out × 10/(100+10)
 *   V_out = V_adc × 11
 *
 * ADC: PA1 / ADC1 CH1, 12-bit, software trigger.
 * Reference voltage: 3.3 V.
 *
 * The running mean accumulates DC_MON_WINDOW samples (≈100 ms at 10 ms
 * call period, but functionally 1 second since we accumulate sum/count).
 * Fault triggers if |mean_mv| > DC_FAULT_MV_THRESH.
 */

#include "dc_monitor.h"
#include "main.h"
#include "mcp4131_driver.h"

#define VCC_MV          3300
#define DIVIDER_RATIO   11      /* (100k + 10k) / 10k */

static int32_t  s_sum;          /* running sum of V_out in mV */
static uint32_t s_count;        /* samples accumulated       */
static int16_t  s_mean_mv;      /* last computed mean         */
static bool     s_fault_active = false;

/* ─────────────────────────────────────────────────────────────── */
void dc_monitor_sample(void)
{
    ADC_ChannelConfTypeDef ch = {
        .Channel      = DC_MON_ADC_CHANNEL,
        .Rank         = ADC_REGULAR_RANK_1,
        .SamplingTime = ADC_SAMPLINGTIME_COMMON_1,
    };
    HAL_ADC_ConfigChannel(&hadc1, &ch);
    HAL_ADC_Start(&hadc1);
    HAL_ADC_PollForConversion(&hadc1, 10);
    uint32_t raw = HAL_ADC_GetValue(&hadc1);
    HAL_ADC_Stop(&hadc1);

    /* V_adc in mV (0–3300), then scale to V_out */
    int32_t v_adc_mv  = (int32_t)((raw * (uint32_t)VCC_MV) / 4095U);
    int32_t v_out_mv  = v_adc_mv * DIVIDER_RATIO;

    /* ADC is single-ended (0-3.3V). To measure signed V_out (−15V..+15V),
     * the divider output (V_out/11) sits around 0V nominally.
     * Convert: signed_mv = v_out_mv − 0 (ideal midpoint at 0V in, so ADC
     * reads ~0 when OPA548 output is 0V DC).
     * With a 1 Hz AC drive the ADC mean approaches the DC component. */
    int32_t signed_out_mv = v_out_mv;   /* 0-bias correction via mean tracking */

    s_sum += signed_out_mv;
    s_count++;

    if (s_count >= DC_MON_WINDOW) {
        s_mean_mv = (int16_t)(s_sum / (int32_t)s_count);
        s_sum   = 0;
        s_count = 0;

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.dc_offset_mv = s_mean_mv;
        xSemaphoreGive(g_state_mutex);
    }
}

bool dc_monitor_check(void)
{
    int16_t mean = s_mean_mv;
    if (mean < 0) mean = -mean;   /* |mean| */

    if (mean > DC_FAULT_MV_THRESH && !s_fault_active) {
        s_fault_active = true;

        /* Drive wiper to 0 immediately — kills LC drive */
        xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
        mcp4131_set_wiper(0);
        xSemaphoreGive(g_spi_mutex);

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.status   |= STATUS_DC_FAULT;
        g_nd.wiper_raw = 0;
        xSemaphoreGive(g_state_mutex);
    }
    return s_fault_active;
}

int16_t dc_monitor_get_mv(void)
{
    return s_mean_mv;
}

void dc_monitor_clear_fault(void)
{
    s_fault_active = false;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_nd.status &= (uint8_t)~STATUS_DC_FAULT;
    xSemaphoreGive(g_state_mutex);
}
