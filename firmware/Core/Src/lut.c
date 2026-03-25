/* SPDX-License-Identifier: GPL-2.0-only
 * lut.c — Flash-backed calibration LUT with bilinear interpolation
 *
 * LUT layout in SRAM and flash:
 *   lut_sram[LUT_TEMP_POINTS][LUT_VOLT_POINTS]  (uint16_t, nd_x100)
 *
 * Flash page format at LUT_FLASH_ADDR (2 KB page):
 *   [magic : u16][lut : u8×LUT_SIZE][crc16 : u16][padding to 2 KB]
 *
 * CRC-16/CCITT over the lut bytes only.
 */

#include "lut.h"
#include "main.h"
#include "mcp4131_driver.h"
#include <string.h>
#include <math.h>

/* ── SRAM copy of the LUT ───────────────────────────────────────── */
static uint16_t s_lut[LUT_TEMP_POINTS][LUT_VOLT_POINTS];
static bool     s_lut_valid = false;

/* ── CRC-16/CCITT (poly 0x1021, init 0xFFFF) ───────────────────── */
static uint16_t crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    }
    return crc;
}

/* ─────────────────────────────────────────────────────────────── */
bool lut_load(void)
{
    const uint8_t *flash = (const uint8_t *)LUT_FLASH_ADDR;

    uint16_t magic = (uint16_t)flash[0] | ((uint16_t)flash[1] << 8);
    if (magic != LUT_MAGIC) {
        s_lut_valid = false;
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.status &= (uint8_t)~STATUS_LUT_VALID;
        xSemaphoreGive(g_state_mutex);
        return false;
    }

    const uint8_t *lut_bytes = flash + 8;  /* magic stored as 8-byte doubleword */
    uint16_t stored_crc = (uint16_t)lut_bytes[LUT_SIZE]
                        | ((uint16_t)lut_bytes[LUT_SIZE + 1] << 8);
    uint16_t calc_crc = crc16(lut_bytes, LUT_SIZE);

    if (stored_crc != calc_crc) {
        s_lut_valid = false;
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.status &= (uint8_t)~STATUS_LUT_VALID;
        xSemaphoreGive(g_state_mutex);
        return false;
    }

    memcpy(s_lut, lut_bytes, LUT_SIZE);
    s_lut_valid = true;

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_nd.status |= STATUS_LUT_VALID;
    xSemaphoreGive(g_state_mutex);
    return true;
}

bool lut_write(const uint16_t data[LUT_TEMP_POINTS][LUT_VOLT_POINTS])
{
    uint16_t crc = crc16((const uint8_t *)data, LUT_SIZE);

    HAL_FLASH_Unlock();

    /* Erase the 2 KB page */
    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Page      = (LUT_FLASH_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE,
        .NbPages   = 1,
    };
    uint32_t page_error = 0;
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Write header: magic */
    uint32_t addr = LUT_FLASH_ADDR;
    uint64_t magic_word = ((uint64_t)LUT_MAGIC) | 0xFFFFFFFF00000000ULL;
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, magic_word);
    addr += 8;

    /* Write LUT data in 8-byte (doubleword) chunks */
    const uint8_t *src = (const uint8_t *)data;
    size_t remaining = LUT_SIZE;
    while (remaining >= 8) {
        uint64_t dword;
        memcpy(&dword, src, 8);
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dword);
        addr    += 8;
        src     += 8;
        remaining -= 8;
    }
    if (remaining > 0) {
        /* Pad last chunk with 0xFF */
        uint64_t dword = 0xFFFFFFFFFFFFFFFFULL;
        memcpy(&dword, src, remaining);
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, dword);
        addr += 8;
    }

    /* Write CRC (2 bytes, padded to 8) */
    uint64_t crc_word = ((uint64_t)crc) | 0xFFFFFFFFFFFF0000ULL;
    HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, crc_word);

    HAL_FLASH_Lock();

    /* Update SRAM copy */
    memcpy(s_lut, data, LUT_SIZE);
    s_lut_valid = true;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_nd.status |= STATUS_LUT_VALID;
    xSemaphoreGive(g_state_mutex);
    return true;
}

/* ─────────────────────────────────────────────────────────────── *
 * Bilinear interpolation                                          *
 * ─────────────────────────────────────────────────────────────── */
uint16_t lut_interpolate(uint16_t v_rms_mv, int16_t temp_c10)
{
    if (!s_lut_valid)
        return 110;   /* return minimum ND if no LUT */

    /* ── Temperature axis index ───────────────────────────────── */
    /* lut_temp_axis: 50, 150, 250, 350, 450  (°C × 10) */
    int ti = 0;
    for (int i = 0; i < LUT_TEMP_POINTS - 1; i++) {
        if (temp_c10 >= lut_temp_axis[i])
            ti = i;
    }
    /* Clamp to valid range */
    if (temp_c10 < lut_temp_axis[0])         ti = 0;
    if (temp_c10 >= lut_temp_axis[LUT_TEMP_POINTS - 1]) ti = LUT_TEMP_POINTS - 2;

    float t_frac = 0.0f;
    int16_t t0 = lut_temp_axis[ti];
    int16_t t1 = lut_temp_axis[ti + 1];
    if (t1 != t0)
        t_frac = (float)(temp_c10 - t0) / (float)(t1 - t0);

    /* ── Voltage axis index ────────────────────────────────────── *
     * step = 250 mV (0.25 V).  vi = v_rms_mv / 250               */
    int vi = (int)(v_rms_mv / 250);
    if (vi >= LUT_VOLT_POINTS - 1) vi = LUT_VOLT_POINTS - 2;
    if (vi < 0)                    vi = 0;

    float v_frac = (float)(v_rms_mv - vi * 250) / 250.0f;

    /* ── Bilinear blend ────────────────────────────────────────── */
    float q00 = s_lut[ti    ][vi    ];
    float q10 = s_lut[ti + 1][vi    ];
    float q01 = s_lut[ti    ][vi + 1];
    float q11 = s_lut[ti + 1][vi + 1];

    float r0  = q00 + t_frac * (q10 - q00);
    float r1  = q01 + t_frac * (q11 - q01);
    return (uint16_t)(r0 + v_frac * (r1 - r0) + 0.5f);
}

uint16_t lut_inverse(uint16_t nd_x100, int16_t temp_c10)
{
    if (!s_lut_valid)
        return 0;

    /* Find temperature row */
    int ti = 0;
    for (int i = 0; i < LUT_TEMP_POINTS - 1; i++)
        if (temp_c10 >= lut_temp_axis[i]) ti = i;
    if (temp_c10 < lut_temp_axis[0])                   ti = 0;
    if (temp_c10 >= lut_temp_axis[LUT_TEMP_POINTS - 1]) ti = LUT_TEMP_POINTS - 1;

    /* Linear search along voltage axis for the nd value */
    for (int vi = 0; vi < LUT_VOLT_POINTS - 1; vi++) {
        if (s_lut[ti][vi] <= nd_x100 && s_lut[ti][vi + 1] >= nd_x100) {
            /* Linear interpolation between volt steps */
            float nd0 = s_lut[ti][vi];
            float nd1 = s_lut[ti][vi + 1];
            float frac = (nd1 != nd0) ? (nd_x100 - nd0) / (nd1 - nd0) : 0.0f;
            return (uint16_t)((vi + frac) * 250.0f + 0.5f);
        }
    }
    /* Clamp to maximum voltage if nd_x100 > any calibrated value */
    return (uint16_t)((LUT_VOLT_POINTS - 1) * 250);
}

/* ─────────────────────────────────────────────────────────────── *
 * V_rms → wiper mapping                                          *
 *                                                                 *
 * Theoretical relationship (to be replaced by Phase 1.6 table):  *
 *   At wiper=0,   Rin ≈ 10 kΩ → G = 1 + 91k/10k = 10.1         *
 *   At wiper=127, Rin ≈ 75 Ω  → G ≫ 10.1 (clipped to ±15V)     *
 *                                                                 *
 * Wait — per INTEGRATION_MAP.md spec:                            *
 *   wiper=127 = 10 kΩ → G = 10.1  (full scale)                  *
 *   wiper=0   → G = 1             (minimum, ±1.65V out)          *
 *                                                                 *
 * We implement this as: V_rms(wiper) ≈ 1650 + wiper × 105 mV     *
 * (linear from 1650 mV @ wiper=0 to 15000 mV @ wiper=127)       *
 * TODO: replace with characterised data from Phase 1.6.          *
 * ─────────────────────────────────────────────────────────────── */
uint8_t lut_vrms_to_wiper(uint16_t v_rms_mv)
{
#define VRMS_MIN_MV 1650U
#define VRMS_MAX_MV 15000U
    if (v_rms_mv <= VRMS_MIN_MV) return 0;
    if (v_rms_mv >= VRMS_MAX_MV) return 127;
    return (uint8_t)(((uint32_t)(v_rms_mv - VRMS_MIN_MV) * 127U)
                     / (VRMS_MAX_MV - VRMS_MIN_MV));
}

/* ND_TARGET_u8 (0-255) → CCM index (0-60), linear */
uint8_t lut_nd_to_ccm_idx(uint8_t nd_target_u8)
{
    return (uint8_t)(((uint32_t)nd_target_u8 * 60U) / 255U);
}
