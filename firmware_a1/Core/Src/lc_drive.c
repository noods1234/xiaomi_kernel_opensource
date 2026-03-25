/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * lc_drive.c — LC cell AC drive controller
 *
 * ONE INCH WONDER — Rev A-1 eND Controller Firmware
 * Document: LCDRV-A1-FW-001 Rev A  |  2026-03-25
 *
 * Drive topology (replaces AD9833 DDS + OPA548 from LCDRV-001):
 *
 *   STM32 TIM2_CH1 → ADG1419 IN (SPDT polarity switch)
 *                         │
 *              ┌──────────┴──────────┐
 *              S1 (TIM2_CH1 HIGH)    S2 (TIM2_CH1 LOW)
 *              │                     │
 *         LC+ terminal           OPA2197 Ch.B output
 *              │                  (inverted, or –V rail)
 *         OPA2197 Ch.A ←── AD5696R DAC_A (amplitude setpoint)
 *
 *   Result: AC square wave at LC cell with amplitude set by DAC_A.
 *   Carrier frequency: TIM2 ARR, default 500 Hz (characterise with cell).
 *
 * For an AC square wave: Vrms = Vpeak = OPA2197 output = DAC × OPA_gain
 * So lc_drive_set_vrms() maps directly to the DAC code.
 *
 * TIM2 configuration (set up by CubeMX / HAL_TIM_OC_Start):
 *   - Clock: 160 MHz (STM32U5 PLL1P / prescaler as needed)
 *   - ARR: (SystemCoreClock / prescaler / frequency) − 1
 *   - OC mode: toggle on match (TIM_OCMODE_TOGGLE)
 *   - CCR1 = ARR / 2 (50% duty cycle)
 *   - Output pin: TIM2_CH1 → PA0 (or board-specific GPIO, verify pinout)
 *
 * TPS65131 enable:
 *   - GPIO PA8 (RAIL_EN) to TPS65131 EN1
 *   - GPIO PA9 (RAIL_NEG_EN) to TPS65131 EN2 (+15V first, then -15V)
 *   - Sequenced in lc_drive_enable() / lc_drive_disable()
 */

#include "main.h"

#include "ad5696r_driver.h"

/* ── GPIO for TPS65131 rail enable ───────────────────────────── *
 * Adjust port/pin to match board layout.                          */
#define RAIL_POS_GPIO_PORT      GPIOA
#define RAIL_POS_GPIO_PIN       GPIO_PIN_8   /* +15V enable */
#define RAIL_NEG_GPIO_PORT      GPIOA
#define RAIL_NEG_GPIO_PIN       GPIO_PIN_9   /* -15V enable */

/* ── TIM2 carrier frequency ──────────────────────────────────── *
 * 500 Hz default. Characterise with the actual LC-Tec cell:      *
 *   - too low  → visible flicker under rolling shutter           *
 *   - too high → capacitive loading limits amplitude             *
 * LC-Tec documentation uses AC square-wave test conditions;      *
 * 100-1000 Hz is the expected empirical range.                   */
#define LC_CARRIER_HZ           500u

/* ── Rail power-up settling time ─────────────────────────────── */
#define RAIL_SETTLE_MS          5u    /* TPS65131 startup time */

/* ── Static state ────────────────────────────────────────────── */
static bool    s_enabled   = false;
static uint32_t s_vrms_mv  = 0;

/* ─────────────────────────────────────────────────────────────── *
 * Private helpers                                                 *
 * ─────────────────────────────────────────────────────────────── */

static void rails_enable(void)
{
    /* Sequence: +15V first, wait, then -15V */
    HAL_GPIO_WritePin(RAIL_POS_GPIO_PORT, RAIL_POS_GPIO_PIN, GPIO_PIN_SET);
    vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
    HAL_GPIO_WritePin(RAIL_NEG_GPIO_PORT, RAIL_NEG_GPIO_PIN, GPIO_PIN_SET);
    vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
}

static void rails_disable(void)
{
    /* Sequence: -15V first, then +15V */
    HAL_GPIO_WritePin(RAIL_NEG_GPIO_PORT, RAIL_NEG_GPIO_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(RAIL_POS_GPIO_PORT, RAIL_POS_GPIO_PIN, GPIO_PIN_RESET);
}

/* ─────────────────────────────────────────────────────────────── *
 * Public API                                                      *
 * ─────────────────────────────────────────────────────────────── */

/**
 * lc_drive_init() — initialise drive hardware, outputs at safe state.
 *
 * Called from main before vTaskStartScheduler().
 * - Configures TIM2 for carrier frequency but does NOT start it.
 * - Ensures TPS65131 enable pins are deasserted (rails off).
 * - Ensures DAC at 0 (ad5696r_init() must have been called first).
 */
void lc_drive_init(void)
{
    /* Ensure rails are off */
    HAL_GPIO_WritePin(RAIL_POS_GPIO_PORT, RAIL_POS_GPIO_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(RAIL_NEG_GPIO_PORT, RAIL_NEG_GPIO_PIN, GPIO_PIN_RESET);

    /*
     * TIM2 prescaler and ARR for LC_CARRIER_HZ square wave.
     * TIM2 runs from APB1 timer clock (≈160 MHz on STM32U5 at max speed).
     * Toggle mode: timer toggles output each ARR match, so one full period
     * = 2 × (ARR+1) × (PSC+1) / Fclk.
     * Choose PSC=159 → tick = 1µs.  ARR = (1e6 / (2 × 500)) - 1 = 999.
     *
     * CubeMX should set this up; replicate here in case of reinitialisation.
     */
    htim2.Instance->PSC = 159;             /* 160 MHz / 160 = 1 MHz tick */
    htim2.Instance->ARR = (1000000u / (2u * LC_CARRIER_HZ)) - 1u;  /* 999 */
    htim2.Instance->CCR1 = htim2.Instance->ARR / 2u;  /* 50% duty, toggle */

    s_enabled  = false;
    s_vrms_mv  = 0;
}

/**
 * lc_drive_enable() — power up rails and start carrier.
 *
 * Call after ad5696r_set_vrms_mv() has been set to the desired value.
 * Caller must NOT hold g_i2c_mutex (rails_enable uses vTaskDelay).
 */
void lc_drive_enable(void)
{
    if (s_enabled)
        return;

    rails_enable();
    HAL_TIM_OC_Start(&htim2, TIM_CHANNEL_1);
    s_enabled = true;
}

/**
 * lc_drive_disable() — stop carrier and power down rails.
 *
 * Sets DAC to 0 first, then stops timer, then disables rails.
 * Caller must hold g_i2c_mutex when calling ad5696r_set_zero().
 */
void lc_drive_disable(void)
{
    if (!s_enabled)
        return;

    /* 1. Zero the DAC (LC cell voltage drops to 0) */
    xSemaphoreTake(g_i2c_mutex, portMAX_DELAY);
    ad5696r_set_zero();
    xSemaphoreGive(g_i2c_mutex);

    /* 2. Stop the AC carrier */
    HAL_TIM_OC_Stop(&htim2, TIM_CHANNEL_1);

    /* 3. De-sequence the ±15V rails */
    rails_disable();

    s_enabled = false;
    s_vrms_mv = 0;
}

/**
 * lc_drive_set_vrms() — update LC cell drive amplitude.
 *
 * Can be called while drive is running; DAC update is glitch-free
 * because the AD5696R holds the previous code until the new one arrives.
 * Caller must hold g_i2c_mutex.
 *
 * @vrms_mv: target cell voltage in millivolts
 * @code_out: if non-NULL, receives DAC code written
 * Returns true on success.
 */
bool lc_drive_set_vrms(uint32_t vrms_mv, uint16_t *code_out)
{
    bool ok = ad5696r_set_vrms_mv(vrms_mv, code_out);
    if (ok)
        s_vrms_mv = vrms_mv;
    return ok;
}

/**
 * lc_drive_fault_shutdown() — immediate safe-state on hardware fault.
 *
 * Hard path: zero DAC, stop timer, disable rails without any delays.
 * Called from DC monitor or critical-temperature handler where scheduler
 * may not be running (or we cannot afford yield latency).
 * Does NOT take g_i2c_mutex — safe because fault state is unrecoverable
 * until host sends a recovery command.
 */
void lc_drive_fault_shutdown(void)
{
    (void)ad5696r_set_zero();          /* best-effort, no lock */
    (void)ad5696r_powerdown();
    HAL_TIM_OC_Stop(&htim2, TIM_CHANNEL_1);
    rails_disable();
    s_enabled = false;
    s_vrms_mv = 0;
}

/**
 * lc_drive_get_vrms() — return current commanded Vrms.
 */
uint32_t lc_drive_get_vrms(void)
{
    return s_vrms_mv;
}

bool lc_drive_is_enabled(void)
{
    return s_enabled;
}
