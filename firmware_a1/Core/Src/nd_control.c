/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nd_control.c — ND setpoint management (Rev A-1)
 *
 * ONE INCH WONDER — Rev A-1 eND Controller Firmware
 * Document: LCDRV-A1-FW-001 Rev A  |  2026-03-25
 *
 * Delta from LCDRV-001 nd_control.c:
 *   - Amplitude set via lc_drive_set_vrms() → ad5696r_set_vrms_mv()
 *     instead of lut_vrms_to_wiper() → mcp4131_set_wiper()
 *   - lc_drive_enable() / lc_drive_disable() replace direct SPI ops
 *   - dac_code stored in g_nd for telemetry
 *   - DC fault recovery also calls lc_drive_enable() on resumption
 */

#include "main.h"
#include "lut.h"
#include "lc_drive.h"
#include "dc_monitor.h"

/* ── Settle timer ────────────────────────────────────────────── */
static uint8_t s_settle_ticks = 0;

/**
 * nd_set_target() — command a new ND level (0-255 raw).
 *
 * Flow:
 *   1. Clear any existing DC fault (host-initiated retry)
 *   2. Look up target Vrms from flash LUT (inverse interpolation)
 *   3. Set LC drive amplitude via DAC
 *   4. Enable drive if it was off (fault recovery)
 *   5. Arm settle timer
 *
 * Called from usb_cdc_handler dispatch while holding g_state_mutex.
 * Takes g_i2c_mutex internally for the DAC write.
 */
void nd_set_target(uint8_t nd_u8)
{
    /* 1. Clear DC fault to allow retry */
    dc_monitor_clear_fault();
    g_nd.status &= (uint8_t)~STATUS_DC_FAULT;

    /* 2. LUT inverse: nd_u8 → Vrms_mv */
    uint32_t vrms_mv = 0;
    if (g_nd.status & STATUS_LUT_VALID) {
        vrms_mv = lut_inverse(nd_u8, (int16_t)g_nd.temp_decidegc);
    } else {
        /*
         * LUT not valid: linear fallback.
         * nd_u8=0   → 1650mV, nd_u8=255 → 15000mV
         * Vrms = 1650 + nd_u8 × (15000-1650) / 255
         */
        vrms_mv = 1650u + ((uint32_t)nd_u8 * 13350u) / 255u;
    }

    /* 3. Set amplitude */
    uint16_t code = 0;
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    bool ok = lc_drive_set_vrms(vrms_mv, &code);
    xSemaphoreGive(g_i2c_mutex);

    if (!ok) {
        /* I2C fault — do not arm drive */
        return;
    }

    g_nd.nd_target = (int16_t)nd_u8;
    g_nd.dac_code  = code;
    g_nd.ccm_idx   = lut_nd_to_ccm_idx(nd_u8);

    /* 4. Enable drive if rails were off (power-on or fault recovery) */
    if (!lc_drive_is_enabled())
        lc_drive_enable();

    /* 5. Arm settle timer */
    g_nd.status &= (uint8_t)~STATUS_SETTLED;
    s_settle_ticks = SETTLE_TICKS;
}

/**
 * nd_tick_10ms() — called from control_task every 10 ms.
 * Decrements settle counter; sets STATUS_SETTLED when it reaches 0.
 */
void nd_tick_10ms(void)
{
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);

    if (s_settle_ticks > 0) {
        --s_settle_ticks;
        if (s_settle_ticks == 0)
            g_nd.status |= STATUS_SETTLED;
    }

    xSemaphoreGive(g_state_mutex);
}
