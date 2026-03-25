/* SPDX-License-Identifier: GPL-2.0-only
 * lut.h — Flash-backed calibration LUT
 *
 * Structure: lut[LUT_TEMP_POINTS][LUT_VOLT_POINTS] → nd_stops × 100
 *
 * Temperature axis: 5 points at 5, 15, 25, 35, 45 °C  (10 °C steps)
 * Voltage axis:    61 points at 0.0 to 15.0 V in 0.25 V steps
 *
 * Flash location:  0x0807F800 (last 2 KB page of 512 KB flash)
 * Page size:       2 KB — excluded from OTA erase in linker script
 * Format:          [magic:u16][lut_data:u8×LUT_SIZE][crc16:u16]
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LUT_TEMP_POINTS     5
#define LUT_VOLT_POINTS     61      /* 0.0 V to 15.0 V, 0.25 V steps */
#define LUT_SIZE            (LUT_TEMP_POINTS * LUT_VOLT_POINTS * sizeof(uint16_t))

#define LUT_FLASH_ADDR      0x0807F800UL
#define LUT_MAGIC           0xBEEFU

/* Temperature breakpoints in °C × 10 */
static const int16_t lut_temp_axis[LUT_TEMP_POINTS] = { 50, 150, 250, 350, 450 };

/* Voltage breakpoints × 100 (mV ×10 rounded) */
#define LUT_VOLT_STEP_MV100 25      /* 0.25 V = 25 × 0.01 V */

/* Load LUT from flash into SRAM; verifies checksum.
 * Returns true if valid LUT found. Sets STATUS_LUT_VALID. */
bool lut_load(void);

/* Burn a new LUT to flash. data must be LUT_SIZE bytes (uint16_t array,
 * row-major [temp][volt]). Erases page, writes, verifies. */
bool lut_write(const uint16_t data[LUT_TEMP_POINTS][LUT_VOLT_POINTS]);

/* Bilinear interpolation: returns nd_stops × 100.
 * v_rms_mv: RMS voltage in mV (0–15000)
 * temp_c10: temperature in °C × 10 (e.g. 245 = 24.5 °C) */
uint16_t lut_interpolate(uint16_t v_rms_mv, int16_t temp_c10);

/* Inverse lookup: given nd_stops × 100 and temperature, returns V_rms in mV.
 * Linear search along voltage axis at the nearest temperature row. */
uint16_t lut_inverse(uint16_t nd_x100, int16_t temp_c10);

/* Convert V_rms (mV) to MCP4131 wiper position (0–127).
 * Uses a linear approximation until Phase 1.6 characterisation is complete.
 * TODO: replace with empirical table from Phase 1.6. */
uint8_t  lut_vrms_to_wiper(uint16_t v_rms_mv);

/* Simple ND → CCM index (0–60). Linear mapping over 5.90-stop range. */
uint8_t  lut_nd_to_ccm_idx(uint8_t nd_target_u8);
