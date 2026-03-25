/* SPDX-License-Identifier: GPL-2.0-only
 * dc_monitor.h — OPA548 output DC offset monitor
 *
 * Hardware: R7(100kΩ)/R8(10kΩ) divider on OPA548 OUT → PA1 / ADC1 CH1.
 * V_out_full = V_adc × (100+10)/10 = V_adc × 11
 *
 * Fault condition: |running mean over 100 samples (≈1 s)| > 15 mV DC.
 * On fault: sets STATUS_DC_FAULT, drives MCP4131 wiper to 0 (safe).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* ADC channel for output monitor (PA1) */
#define DC_MON_ADC_CHANNEL  ADC_CHANNEL_1

/* Number of samples in running mean window */
#define DC_MON_WINDOW       100

/* Sample one ADC point and update the running mean.
 * Call at ~10 ms intervals from control_task. */
void dc_monitor_sample(void);

/* Check fault condition against DC_FAULT_MV_THRESH.
 * Returns true if fault; sets STATUS_DC_FAULT and wiper→0 on first fault. */
bool dc_monitor_check(void);

/* Return current running mean in mV (signed). */
int16_t dc_monitor_get_mv(void);

/* Clear fault flag (called on valid ND_TARGET command from host). */
void dc_monitor_clear_fault(void);
