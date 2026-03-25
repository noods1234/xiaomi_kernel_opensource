/* SPDX-License-Identifier: GPL-2.0-only
 * nd_control.c — ND attenuation setpoint control state machine
 *
 * nd_set_target():
 *   1. Inverse LUT lookup: nd_x100 + temp → V_rms_mv
 *   2. V_rms → wiper position
 *   3. Write wiper via MCP4131
 *   4. Update g_nd.nd_actual, g_nd.ccm_idx
 *   5. Start 15 ms settle timer (STATUS_SETTLED cleared)
 *
 * nd_tick_10ms():
 *   Decrements settle timer; sets STATUS_SETTLED when expired.
 */

#include "nd_control.h"
#include "main.h"
#include "mcp4131_driver.h"
#include "lut.h"
#include "dc_monitor.h"
#include "temp_sense.h"

/* Settle timer in 10 ms ticks (15 ms → 2 ticks, rounded up) */
#define SETTLE_TICKS    2

static volatile uint8_t s_settle_count = 0;

/* ─────────────────────────────────────────────────────────────── */
void nd_set_target(uint8_t nd_target_u8)
{
    /* Ignore commands if DC fault is active */
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    bool fault = !!(g_nd.status & STATUS_DC_FAULT);
    xSemaphoreGive(g_state_mutex);
    if (fault)
        return;

    /* Clear fault counter on valid command (per dc_monitor spec) */
    dc_monitor_clear_fault();

    /* Lookup: nd_u8 → nd_x100 → V_rms */
    uint16_t nd_x100  = 110U + (uint16_t)(((uint32_t)nd_target_u8 * 590U) / 255U);
    int16_t  temp_c10 = temp_sense_get();

    uint16_t v_rms_mv = lut_inverse(nd_x100, temp_c10);
    uint8_t  wiper    = lut_vrms_to_wiper(v_rms_mv);

    /* Write wiper */
    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    mcp4131_set_wiper(wiper);
    xSemaphoreGive(g_spi_mutex);

    /* Update state */
    uint8_t ccm_idx = lut_nd_to_ccm_idx(nd_target_u8);

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_nd.nd_target  = nd_target_u8;
    g_nd.nd_actual  = nd_target_u8;    /* predicted actual = target (LUT-corrected) */
    g_nd.ccm_idx    = ccm_idx;
    g_nd.wiper_raw  = wiper;
    g_nd.status    &= (uint8_t)~STATUS_SETTLED;   /* clear until settle timer expires */
    xSemaphoreGive(g_state_mutex);

    /* Arm settle timer */
    __atomic_store_n(&s_settle_count, SETTLE_TICKS, __ATOMIC_RELAXED);
}

void nd_tick_10ms(void)
{
    uint8_t cnt = __atomic_load_n(&s_settle_count, __ATOMIC_RELAXED);
    if (cnt == 0)
        return;

    cnt--;
    __atomic_store_n(&s_settle_count, cnt, __ATOMIC_RELAXED);

    if (cnt == 0) {
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.status |= STATUS_SETTLED;
        xSemaphoreGive(g_state_mutex);
    }
}
