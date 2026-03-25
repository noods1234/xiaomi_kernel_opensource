/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc_monitor.c — LC cell DC offset monitor (Rev A-1)
 *
 * ONE INCH WONDER — Rev A-1 eND Controller Firmware
 * Document: LCDRV-A1-FW-001 Rev A  |  2026-03-25
 *
 * Monitors the DC component of the LC cell drive voltage via a
 * resistive divider on STM32U5 ADC1.  Any sustained DC offset
 * damages the LC cell; the threshold is 15 mV (same as LCDRV-001).
 *
 * Delta from LCDRV-001 dc_monitor.c:
 *   - Fault action: lc_drive_fault_shutdown() instead of mcp4131_set_wiper(0)
 *     The shutdown zeroes the DAC, stops the timer, and disables TPS65131.
 *   - ADC reference: STM32U5 ADC1 uses internal VREF+ = 3.3V (14-bit)
 *     vs STM32G0B1 12-bit ADC.  Scale factor updated accordingly.
 *
 * Hardware:
 *   LC cell midpoint → R_high (100kΩ) → ADC1_IN1 → R_low (100kΩ) → GND
 *   Divider ratio = 0.5  →  V_adc = V_cell_mid / 2
 *   V_cell_mid = V_adc × 2
 *
 *   For ±15V drive, the midpoint should be 0V DC.  Any offset is measured
 *   by converting ADC reading relative to mid-scale (VREF/2 = 1.65V).
 */

#include "main.h"
#include "lc_drive.h"

/* ── ADC configuration ───────────────────────────────────────── */
#define ADC_RESOLUTION_BITS     14u
#define ADC_FULL_SCALE          (1u << ADC_RESOLUTION_BITS)  /* 16384 */
#define ADC_VREF_MV             3300u   /* VREF+ = 3.3V */

/* DC offset divider: V_cell_mid = V_adc × 2 */
#define DIVIDER_FACTOR          2u

/* ── Running mean ────────────────────────────────────────────── */
#define DC_MONITOR_SAMPLES      100u
#define DC_FAULT_THRESHOLD_MV   15u     /* |mean| > 15 mV → fault */

static int32_t s_accumulator  = 0;
static uint32_t s_sample_count = 0;
static int32_t  s_mean_mv      = 0;
static bool     s_fault_active = false;

/* ─────────────────────────────────────────────────────────────── *
 * Public API                                                      *
 * ─────────────────────────────────────────────────────────────── */

/**
 * dc_monitor_sample() — take one ADC sample and update running mean.
 * Called from control_task every 10 ms.
 */
void dc_monitor_sample(void)
{
    if (s_fault_active)
        return;

    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 5) != HAL_OK)
        return;

    uint32_t raw = HAL_ADC_GetValue(&hadc1);

    /*
     * Convert to millivolts relative to midpoint (VREF/2):
     *   V_adc_mv = raw × VREF_MV / ADC_FULL_SCALE
     *   offset_mv = (V_adc_mv − VREF_MV/2) × DIVIDER_FACTOR
     *
     * Using integer arithmetic (raw ≤ 16383, VREF_MV=3300):
     *   V_adc_mv × 2 = raw × 3300 × 2 / 16384 — fits in 32 bits
     */
    int32_t v_adc_x2_mv = (int32_t)((raw * ADC_VREF_MV * 2u) / ADC_FULL_SCALE);
    int32_t offset_mv   = (v_adc_x2_mv - (int32_t)ADC_VREF_MV) *
                          (int32_t)DIVIDER_FACTOR;

    s_accumulator += offset_mv;
    ++s_sample_count;

    if (s_sample_count >= DC_MONITOR_SAMPLES) {
        s_mean_mv     = s_accumulator / (int32_t)DC_MONITOR_SAMPLES;
        s_accumulator = 0;
        s_sample_count = 0;

        /* Update telemetry under state lock */
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.dc_offset_mv = (int16_t)s_mean_mv;
        xSemaphoreGive(g_state_mutex);
    }
}

/**
 * dc_monitor_check() — evaluate most recent mean and trip fault.
 * Called from control_task after dc_monitor_sample().
 * Returns true if a new fault was detected (caller should shut down drive).
 */
bool dc_monitor_check(void)
{
    if (s_fault_active)
        return false;

    if (s_sample_count != 0)
        return false;   /* mean not yet updated this cycle */

    int32_t abs_mean = s_mean_mv < 0 ? -s_mean_mv : s_mean_mv;
    if (abs_mean > DC_FAULT_THRESHOLD_MV) {
        s_fault_active = true;
        return true;
    }
    return false;
}

/**
 * dc_monitor_clear_fault() — reset fault state for host-commanded retry.
 * Called from nd_set_target() when host sends a new ND target.
 */
void dc_monitor_clear_fault(void)
{
    s_fault_active  = false;
    s_accumulator   = 0;
    s_sample_count  = 0;
    s_mean_mv       = 0;
}

int32_t dc_monitor_mean_mv(void)
{
    return s_mean_mv;
}
