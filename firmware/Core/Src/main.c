/* SPDX-License-Identifier: GPL-2.0-only
 * main.c — LCDRV-001 STM32G0B1 top-level init and FreeRTOS task creation
 * ONE INCH WONDER — LC Variable ND Filter Driver Firmware
 * Document: LCDRV-FW-001 Rev A  |  2026-03-24
 *
 * Hardware: STM32G0B1KET6 LQFP-32
 *   PA4  → AD9833 FSYNC (CS, active-low)
 *   PA5  → SPI1 SCK  (shared: AD9833 + MCP4131)
 *   PA6  → SPI1 MISO (MCP4131 SDO only; AD9833 has no MISO)
 *   PA7  → SPI1 MOSI (shared)
 *   PB0  → MCP4131 CS (active-low)
 *   PA0  → ADC1 CH0 (NTC)
 *   PA1  → ADC1 CH1 (OPA548 output monitor)
 *   PA11 → USB DM
 *   PA12 → USB DP
 *
 * Generate the CubeMX base project (USB CDC, SPI1, ADC1, FreeRTOS) and
 * add the application sources listed in CMakeLists.txt.
 */

#include "main.h"
#include "usb_cdc_handler.h"
#include "ad9833_driver.h"
#include "mcp4131_driver.h"
#include "lut.h"
#include "temp_sense.h"
#include "dc_monitor.h"
#include "nd_control.h"
#include "pid.h"

/* ── HAL handles (initialised by CubeMX-generated code) ───────── */
SPI_HandleTypeDef  hspi1;
ADC_HandleTypeDef  hadc1;

/* ── FreeRTOS handles ──────────────────────────────────────────── */
SemaphoreHandle_t  g_spi_mutex;
SemaphoreHandle_t  g_state_mutex;
SemaphoreHandle_t  g_usb_rx_sem;

/* ── Global firmware state ─────────────────────────────────────── */
volatile nd_state_t g_nd = {
    .nd_target     = ND_TARGET_SAFE,
    .nd_actual     = ND_TARGET_SAFE,
    .mode          = 0,
    .temp_decidegc = 250,   /* safe default 25.0 °C until first ADC read */
    .status        = 0,
    .ccm_idx       = 0,
    .wiper_raw     = 0,
    .dc_offset_mv  = 0,
};

/* ── Software CS helpers ───────────────────────────────────────── */
void spi_cs_ad9833_assert(void)   { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET); }
void spi_cs_ad9833_deassert(void) { HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);   }
void spi_cs_mcp4131_assert(void)  { HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET); }
void spi_cs_mcp4131_deassert(void){ HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);   }

/* ── FreeRTOS task functions (forward declarations) ───────────── */
static void control_task(void *arg);
static void sensor_task(void *arg);
static void pid_task_fn(void *arg);

/* ─────────────────────────────────────────────────────────────── */
int main(void)
{
    /* CubeMX-generated HAL, clock, and peripheral init */
    HAL_Init();
    SystemClock_Config();   /* defined in generated stm32g0xx_hal_msp.c */
    MX_GPIO_Init();
    MX_SPI1_Init();
    MX_ADC1_Init();
    MX_USB_Device_Init();   /* starts USB CDC stack */

    /* ── RTOS primitives ─────────────────────────────────────── */
    g_spi_mutex   = xSemaphoreCreateMutex();
    g_state_mutex = xSemaphoreCreateMutex();
    g_usb_rx_sem  = xSemaphoreCreateBinary();

    configASSERT(g_spi_mutex);
    configASSERT(g_state_mutex);
    configASSERT(g_usb_rx_sem);

    /* ── Hardware init sequence ──────────────────────────────── *
     * IMPORTANT: MCP4131 must be driven to wiper=0 before cell  *
     * is connected and before AD9833 starts.                     */
    mcp4131_init();     /* sets wiper=0, safe minimum gain        */
    ad9833_init();      /* starts 1 kHz square wave               */
    lut_load();         /* copies flash LUT to SRAM               */

    /* ── Create FreeRTOS tasks ───────────────────────────────── */
    xTaskCreate(usb_task,      "USB",     128, NULL, 4, NULL);  /* 512B  */
    xTaskCreate(control_task,  "CTRL",     64, NULL, 3, NULL);  /* 256B  */
    xTaskCreate(sensor_task,   "SENS",     64, NULL, 2, NULL);  /* 256B  */
    /* pid_task created only when mode=2 is activated (see control_task) */

    vTaskStartScheduler();

    /* Never reached */
    while (1) {}
}

/* ── Control task (10 ms periodic) ────────────────────────────── */
static void control_task(void *arg)
{
    (void)arg;
    static TaskHandle_t  pid_task_handle = NULL;
    TickType_t           last_wake       = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));

        dc_monitor_sample();
        dc_monitor_check();
        nd_tick_10ms();

        /* Create / delete pid_task based on current mode */
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        uint8_t mode = g_nd.mode;
        xSemaphoreGive(g_state_mutex);

        if (mode == 2 && pid_task_handle == NULL) {
            xTaskCreate(pid_task_fn, "PID", 64, NULL, 3, &pid_task_handle);
        } else if (mode != 2 && pid_task_handle != NULL) {
            vTaskDelete(pid_task_handle);
            pid_task_handle = NULL;
        }
    }
}

/* ── Sensor task (100 ms periodic) ────────────────────────────── */
static void sensor_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));

        int16_t t = temp_sense_update();

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_nd.temp_decidegc = t;
        if (t > 400)   /* > 40.0 °C */
            g_nd.status |= STATUS_TEMP_WARN;
        else
            g_nd.status &= (uint8_t)~STATUS_TEMP_WARN;
        xSemaphoreGive(g_state_mutex);
    }
}

/* ── PID task (30 ms periodic, mode=2 only) ────────────────────── *
 * In mode=2 the host streams target_ev via a (TBD) streaming cmd;  *
 * for now this task drives the ND toward a fixed target_ev that    *
 * the host writes to a scratch register.  See DQ_3.                */
static void pid_task_fn(void *arg)
{
    (void)arg;
    static pid_state_t  pid     = { 0 };
    static float        target_ev = 14.0f;  /* cinema daylight default */
    TickType_t          last_wake = xTaskGetTickCount();

    pid_reset(&pid);

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(30));

        /* Host-provided target_ev update mechanism is TBD (DQ_3).
         * Placeholder: read from a volatile scratch in g_nd.           */
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        uint8_t  actual_u8 = g_nd.nd_actual;
        uint8_t  mode      = g_nd.mode;
        xSemaphoreGive(g_state_mutex);

        if (mode != 2)
            break;  /* task will be deleted by control_task */

        /* Approximate current ND in stops: nd_stops = 1.10 + raw × 5.90/255 */
        float nd_actual_stops = 1.10f + (actual_u8 / 255.0f) * 5.90f;

        /* error_ev: positive = scene too bright → need more ND */
        float error_ev = target_ev - nd_actual_stops;
        float delta    = pid_update(&pid, error_ev);

        float nd_new = nd_actual_stops + delta;
        if (nd_new < 1.10f) nd_new = 1.10f;
        if (nd_new > 7.00f) nd_new = 7.00f;

        uint8_t nd_u8 = (uint8_t)((nd_new - 1.10f) / 5.90f * 255.0f + 0.5f);
        nd_set_target(nd_u8);
    }

    vTaskDelete(NULL);
}
