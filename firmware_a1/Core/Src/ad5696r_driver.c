/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ad5696r_driver.c — Analog Devices AD5696R quad 16-bit I2C DAC driver
 *
 * ONE INCH WONDER — Rev A-1 eND Controller Firmware
 * Document: LCDRV-A1-FW-001 Rev A  |  2026-03-25
 *
 * Replaces the MCP4131 SPI digital pot + AD9833 DDS pair from LCDRV-001.
 * The AD5696R provides direct 16-bit amplitude control for the LC drive
 * stage, giving 65536 steps over the 0-2.5V output range (internal ref).
 *
 * AD5696R key facts:
 *   - I2C interface, up to 400 kHz
 *   - Internal 2.5V precision reference (RSTSEL → DVDD to enable)
 *   - 4 independent DAC channels: A-D
 *   - 16-bit resolution: Vout = Vref × code / 65536
 *   - Power-on state: output at 0V (code=0) — safe minimum drive
 *   - LDAC pin: tie to GND for immediate update on write; or use
 *     software LDAC command (0x30) for simultaneous multi-channel update
 *
 * I2C address selection (A2,A1,A0 pins):
 *   GND,GND,GND → 0b0001100 = 0x0C (7-bit)
 *   HAL uses 8-bit left-shifted: 0x18
 *
 * Command byte format: [C3:C0][A3:A0]
 *   C = command, A = channel address (0=A, 1=B, 2=C, 3=D, F=all)
 *   Command 0x3: write to input register and update DAC (normal write)
 *   Command 0x4: power-down (V2=PDx, see datasheet)
 *   Command 0x5: hardware LDAC mask
 *   Command 0x7: software reset (POR state)
 *
 * Channel assignments:
 *   DAC_A: ND amplitude setpoint → OPA2197 → LC cell drive
 *   DAC_B: DC offset trim / calibration rail (future use)
 *   DAC_C: reserved (dual-cell channel 2 in future Rev)
 *   DAC_D: reserved
 */

#include "main.h"

#include <string.h>

/* ── I2C address (A2=A1=A0=GND) ─────────────────────────────── */
#define AD5696R_I2C_ADDR        (0x0Cu << 1)  /* HAL 8-bit: 0x18 */
#define AD5696R_I2C_TIMEOUT_MS  10u

/* ── Channel address nibbles ─────────────────────────────────── */
#define AD5696R_CH_A            0x0u
#define AD5696R_CH_B            0x1u
#define AD5696R_CH_C            0x2u
#define AD5696R_CH_D            0x3u
#define AD5696R_CH_ALL          0xFu

/* ── Command nibbles ─────────────────────────────────────────── */
#define AD5696R_CMD_WRITE_UPDATE 0x3u   /* write input + update DAC  */
#define AD5696R_CMD_POWER_DOWN   0x4u   /* power-down output         */
#define AD5696R_CMD_SW_RESET     0x7u   /* software reset to POR     */

/* ── Reference voltage (mV) and output gain ─────────────────── */
#define AD5696R_VREF_MV         2500u   /* internal 2.5V reference   */
#define AD5696R_FULL_SCALE      65536u  /* 2^16                      */

/*
 * OPA2197 gain from DAC output to LC cell drive voltage:
 *   Gain = 1 + R_fb / R_in = 1 + 50kΩ / 10kΩ = 6
 * Vout_max = 2.5V × 6 = 15V = LC_VRMS_MAX_MV
 */
#define OPA_GAIN_NUM            6u
#define OPA_GAIN_DEN            1u

/* ─────────────────────────────────────────────────────────────── *
 * Internal helpers                                                *
 * ─────────────────────────────────────────────────────────────── */

/**
 * ad5696r_write() — write a 16-bit code to one DAC channel.
 * Caller must hold g_i2c_mutex.
 */
static HAL_StatusTypeDef ad5696r_write(uint8_t chan, uint16_t code)
{
    uint8_t buf[3];
    buf[0] = (uint8_t)((AD5696R_CMD_WRITE_UPDATE << 4) | (chan & 0xF));
    buf[1] = (uint8_t)(code >> 8);
    buf[2] = (uint8_t)(code & 0xFF);
    return HAL_I2C_Master_Transmit(&hi2c2, AD5696R_I2C_ADDR,
                                   buf, 3, AD5696R_I2C_TIMEOUT_MS);
}

/* ─────────────────────────────────────────────────────────────── *
 * Public API                                                      *
 * ─────────────────────────────────────────────────────────────── */

/**
 * ad5696r_init() — reset DAC and set all outputs to 0V (safe state).
 *
 * Called before vTaskStartScheduler().  I2C bus must be ready.
 * LDAC pin must be tied to GND for immediate updates.
 * Returns true on success.
 */
bool ad5696r_init(void)
{
    uint8_t reset_cmd[3] = {
        (uint8_t)(AD5696R_CMD_SW_RESET << 4), 0x00, 0x00
    };

    /* Software reset: all outputs to 0 (midscale in bipolar mode,
     * but we use unipolar so 0V = minimum drive = safe). */
    HAL_StatusTypeDef r = HAL_I2C_Master_Transmit(
        &hi2c2, AD5696R_I2C_ADDR, reset_cmd, 3, AD5696R_I2C_TIMEOUT_MS);
    if (r != HAL_OK)
        return false;

    /* Explicitly write zero to DAC_A (ND amplitude) for clarity */
    return ad5696r_write(AD5696R_CH_A, 0) == HAL_OK;
}

/**
 * ad5696r_set_vrms_mv() — set LC cell drive amplitude.
 *
 * Converts target Vrms (millivolts) to a DAC code and writes DAC_A.
 * For an AC square wave, Vpeak = Vrms, so the OPA2197 output peak
 * equals the requested Vrms value.
 *
 * Clamps to [LC_VRMS_MIN_MV, LC_VRMS_MAX_MV].
 * Caller must hold g_i2c_mutex.
 *
 * @vrms_mv: target drive voltage in millivolts
 * @code_out: if non-NULL, receives the computed DAC code (for logging)
 * Returns true on success.
 */
bool ad5696r_set_vrms_mv(uint32_t vrms_mv, uint16_t *code_out)
{
    /* Clamp */
    if (vrms_mv < LC_VRMS_MIN_MV) vrms_mv = LC_VRMS_MIN_MV;
    if (vrms_mv > LC_VRMS_MAX_MV) vrms_mv = LC_VRMS_MAX_MV;

    /*
     * Vdac = Vcell / OPA_GAIN = vrms_mv / 6
     * DAC code = (Vdac / Vref) × 65536
     *           = (vrms_mv / 6 / 2500) × 65536
     *           = vrms_mv × 65536 / (6 × 2500)
     *           = vrms_mv × 65536 / 15000
     *
     * Use 32-bit arithmetic — vrms_mv ≤ 15000, product ≤ 15000 × 65536
     * = 983,040,000 which fits in uint32_t (max 4,294,967,295).
     */
    uint32_t code32 = (vrms_mv * AD5696R_FULL_SCALE) /
                      (OPA_GAIN_NUM * AD5696R_VREF_MV);
    if (code32 > 0xFFFFu) code32 = 0xFFFFu;

    uint16_t code = (uint16_t)code32;
    if (code_out) *code_out = code;

    return ad5696r_write(AD5696R_CH_A, code) == HAL_OK;
}

/**
 * ad5696r_set_zero() — immediately set ND amplitude to 0V.
 *
 * Used on DC fault, TEMP_CRIT, or deliberate safe-state.
 * Caller must hold g_i2c_mutex.
 */
bool ad5696r_set_zero(void)
{
    return ad5696r_write(AD5696R_CH_A, 0) == HAL_OK;
}

/**
 * ad5696r_powerdown() — put DAC_A in power-down mode (output Hi-Z).
 *
 * Called on critical fault where the OPA2197 must be decoupled from
 * the LC cell.  Normal safe-state should use ad5696r_set_zero() first.
 * Caller must hold g_i2c_mutex.
 */
bool ad5696r_powerdown(void)
{
    /*
     * Power-down command: C=0x4, A=channel.
     * D[9:8] = 01 → 1kΩ to GND (lowest-impedance power-down mode,
     * avoids floating the OPA2197 non-inverting input).
     */
    uint8_t buf[3];
    buf[0] = (uint8_t)((AD5696R_CMD_POWER_DOWN << 4) | AD5696R_CH_A);
    buf[1] = 0x01;   /* 1kΩ to GND */
    buf[2] = 0x00;
    return HAL_I2C_Master_Transmit(&hi2c2, AD5696R_I2C_ADDR,
                                   buf, 3, AD5696R_I2C_TIMEOUT_MS) == HAL_OK;
}
