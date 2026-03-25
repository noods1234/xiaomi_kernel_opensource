/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tmp117_driver.c — TI TMP117 high-accuracy I2C temperature sensor driver
 *
 * ONE INCH WONDER — Rev A-1 eND Controller Firmware
 * Document: LCDRV-A1-FW-001 Rev A  |  2026-03-25
 *
 * Replaces the NTC B3950 + STM32 ADC + Steinhart-Hart path from LCDRV-001.
 *
 * TMP117 key facts:
 *   - I2C address: 0x48 (ADDR pin to GND)
 *   - Temperature register 0x00: 16-bit 2's complement, 0.0078125°C/LSB
 *   - Config register 0x01: MOD[11:10], CONV[9:7], AVG[6:5], T/nA[4],
 *                           POL[3], DR/Alert[2], EEPROM_BUSY[1], DATA_READY[0]
 *   - One-shot mode: write MOD=11 → conversion starts, DATA_READY set when done
 *   - Continuous mode: MOD=00, CONV selects cycle time
 *   - Alert pin (ALERT): can be used as data-ready interrupt
 *
 * This driver runs in one-shot mode, polled by the sensor_task at 100 ms.
 * The ALERT pin is wired to a STM32 EXTI line as an optional interrupt;
 * it is configured here but the task uses polling for simplicity.
 */

#include "main.h"

#include <math.h>
#include <string.h>

/* ── TMP117 register addresses ───────────────────────────────── */
#define TMP117_REG_RESULT       0x00
#define TMP117_REG_CONFIG       0x01
#define TMP117_REG_T_HIGH_LIMIT 0x02
#define TMP117_REG_T_LOW_LIMIT  0x03
#define TMP117_REG_EEPROM_UL    0x04
#define TMP117_REG_EEPROM1      0x05
#define TMP117_REG_EEPROM2      0x06
#define TMP117_REG_TEMP_OFFSET  0x07
#define TMP117_REG_EEPROM3      0x08
#define TMP117_REG_DEVICE_ID    0x0F

/* ── TMP117 I2C address (ADDR pin = GND) ─────────────────────── */
#define TMP117_I2C_ADDR         (0x48u << 1)  /* HAL uses 8-bit addr */

/* ── Config register bit fields ──────────────────────────────── */
#define TMP117_CFG_ONESHOT      (0x3u << 10)  /* MOD = 11 */
#define TMP117_CFG_CONTINUOUS   (0x0u << 10)  /* MOD = 00 */
#define TMP117_CFG_AVG_NONE     (0x0u << 5)
#define TMP117_CFG_AVG_8        (0x1u << 5)
#define TMP117_CFG_CONV_125MS   (0x0u << 7)
#define TMP117_CFG_DRDY_PIN_EN  (0x1u << 2)   /* DATA_READY pin active */

/* ── Resolution: 7.8125 millidegrees, stored × 10000 to avoid float */
#define TMP117_LSB_MICRODEGC    78125u         /* 0.0078125°C × 10^7 */

/* ── Timeout for one-shot conversion ─────────────────────────── */
#define TMP117_CONV_TIMEOUT_MS  20u            /* 15.5 ms max + margin */
#define TMP117_I2C_TIMEOUT_MS   10u

/* ── Static state ────────────────────────────────────────────── */
static bool s_initialised = false;

/* ─────────────────────────────────────────────────────────────── *
 * Internal helpers                                                *
 * ─────────────────────────────────────────────────────────────── */

static HAL_StatusTypeDef tmp117_write_reg(uint8_t reg, uint16_t value)
{
    uint8_t buf[3];
    buf[0] = reg;
    buf[1] = (uint8_t)(value >> 8);
    buf[2] = (uint8_t)(value & 0xFF);
    return HAL_I2C_Master_Transmit(&hi2c1, TMP117_I2C_ADDR,
                                   buf, 3, TMP117_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef tmp117_read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t buf[2];
    HAL_StatusTypeDef r;

    /* Write register pointer */
    r = HAL_I2C_Master_Transmit(&hi2c1, TMP117_I2C_ADDR,
                                 &reg, 1, TMP117_I2C_TIMEOUT_MS);
    if (r != HAL_OK)
        return r;

    /* Read 2 bytes MSB first */
    r = HAL_I2C_Master_Receive(&hi2c1, TMP117_I2C_ADDR,
                                buf, 2, TMP117_I2C_TIMEOUT_MS);
    if (r == HAL_OK)
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    return r;
}

/* ─────────────────────────────────────────────────────────────── *
 * Public API                                                      *
 * ─────────────────────────────────────────────────────────────── */

/**
 * tmp117_init() — Configure TMP117 for one-shot mode with 8-sample averaging.
 *
 * Called from main before vTaskStartScheduler().  I2C bus must be ready.
 * Returns true on success (device ID verified), false on bus/ID error.
 */
bool tmp117_init(void)
{
    uint16_t dev_id = 0;
    HAL_StatusTypeDef r;

    /* Verify device ID: TMP117 = 0x0117, TMP117N = 0x0217 */
    r = tmp117_read_reg(TMP117_REG_DEVICE_ID, &dev_id);
    if (r != HAL_OK || (dev_id != 0x0117 && dev_id != 0x0217)) {
        /* Not found or wrong ID — sensor absent or I2C fault */
        return false;
    }

    /*
     * Configure: one-shot mode, 8-sample averaging (takes ~16 ms),
     * DATA_READY pin active high, alert mode (not therm mode).
     * One-shot: write config with MOD=11 to start first conversion.
     */
    r = tmp117_write_reg(TMP117_REG_CONFIG,
                         TMP117_CFG_ONESHOT   |
                         TMP117_CFG_AVG_8     |
                         TMP117_CFG_CONV_125MS|
                         TMP117_CFG_DRDY_PIN_EN);
    if (r != HAL_OK)
        return false;

    s_initialised = true;
    return true;
}

/**
 * tmp117_read_decidegc() — Trigger one-shot conversion and return result.
 *
 * Blocks for up to TMP117_CONV_TIMEOUT_MS waiting for DATA_READY.
 * Must be called while holding g_i2c_mutex.
 *
 * @out: temperature in units of 0.1°C (decidegrees Celsius)
 * Returns true on success, false on I2C error or timeout.
 */
bool tmp117_read_decidegc(int16_t *out)
{
    uint16_t cfg, raw;
    HAL_StatusTypeDef r;
    uint32_t deadline;

    if (!s_initialised)
        return false;

    /* Start one-shot conversion */
    r = tmp117_write_reg(TMP117_REG_CONFIG,
                         TMP117_CFG_ONESHOT   |
                         TMP117_CFG_AVG_8     |
                         TMP117_CFG_CONV_125MS|
                         TMP117_CFG_DRDY_PIN_EN);
    if (r != HAL_OK)
        return false;

    /* Poll DATA_READY bit in config register */
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(TMP117_CONV_TIMEOUT_MS);
    do {
        vTaskDelay(pdMS_TO_TICKS(2));
        r = tmp117_read_reg(TMP117_REG_CONFIG, &cfg);
        if (r != HAL_OK)
            return false;
    } while (!(cfg & 0x0001) &&            /* DATA_READY bit */
             (xTaskGetTickCount() < deadline));

    if (!(cfg & 0x0001))
        return false;   /* timeout */

    /* Read result register */
    r = tmp117_read_reg(TMP117_REG_RESULT, &raw);
    if (r != HAL_OK)
        return false;

    /*
     * Convert: raw is 16-bit 2's complement, 0.0078125°C/LSB.
     * Decidegrees = raw × 78125 / 10000000
     *             = raw × 78125 / 10000000
     *
     * To stay in integer arithmetic:
     *   decidegC = (int16_t)raw × 78125 / 1000000
     * but this overflows int32 for raw near ±32768.
     * Use int64_t intermediate.
     */
    int64_t raw_s = (int64_t)(int16_t)raw;
    int64_t decidegc = (raw_s * 78125LL) / 1000000LL;

    /* Clamp to int16_t range (−3276.8 … 3276.7 °C — well within ±150°C) */
    if (decidegc > 32767)  decidegc = 32767;
    if (decidegc < -32768) decidegc = -32768;

    *out = (int16_t)decidegc;
    return true;
}

/**
 * tmp117_set_alert_threshold() — Programme over-temperature alert.
 *
 * Sets the high-limit register so the ALERT pin asserts when the cell
 * exceeds threshold_decidegc.  Used as a hardware backup to the firmware
 * software check in sensor_task.
 *
 * @threshold_decidegc: threshold in 0.1°C units (e.g. 400 = 40.0°C)
 */
void tmp117_set_alert_threshold(int16_t threshold_decidegc)
{
    /*
     * TMP117 high-limit register has the same encoding as the result:
     * 0.0078125°C/LSB.  Convert decidegC → raw:
     *   raw = threshold_decidegc × 1000000 / 78125
     */
    int32_t raw = ((int32_t)threshold_decidegc * 1000000L) / 78125L;

    /* Clamp to 16-bit */
    if (raw > 32767)  raw = 32767;
    if (raw < -32768) raw = -32768;

    /* Best-effort — ignore I2C error here (alert is a belt, not braces) */
    (void)tmp117_write_reg(TMP117_REG_T_HIGH_LIMIT, (uint16_t)(int16_t)raw);
}
