/* SPDX-License-Identifier: GPL-2.0-only
 * ad9833_driver.c — AD9833 DDS waveform generator driver
 *
 * Outputs a 1 kHz square wave on VOUT (AC-coupled to OPA548 IN+).
 * SPI: Mode 1 (CPOL=0, CPHA=1), 16-bit words, CS=PA4 (FSYNC, active-low).
 *
 * Init sequence:
 *   1. Assert RESET (CTRL = B28|RESET)
 *   2. Write FREQ0 register (28-bit, two sequential 16-bit words)
 *   3. Write PHASE0 register (0 offset)
 *   4. Clear RESET, set OPBITEN (square wave mode)
 */

#include "ad9833_driver.h"
#include "main.h"

/* ── Private: send one 16-bit word to AD9833 ──────────────────── *
 * FSYNC (CS) must be asserted around each word.                   */
static void ad9833_write_word(uint16_t word)
{
    /* AD9833 SPI is MSB-first, 16-bit frame */
    uint8_t tx[2] = { (uint8_t)(word >> 8), (uint8_t)(word & 0xFF) };

    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    spi_cs_ad9833_assert();
    HAL_SPI_Transmit(&hspi1, tx, 2, 10);
    spi_cs_ad9833_deassert();
    xSemaphoreGive(g_spi_mutex);
}

/* ─────────────────────────────────────────────────────────────── */
void ad9833_write_freq(uint32_t freqreg)
{
    /* Enable 28-bit sequential write + assert RESET to hold outputs */
    ad9833_write_word(AD9833_B28 | AD9833_RESET);

    /* Low 14 bits: D15=1 D14=0 (FREQ0 address) | bits[13:0] */
    ad9833_write_word(AD9833_FREQ0_ADDR | (uint16_t)(freqreg & 0x3FFF));

    /* High 14 bits */
    ad9833_write_word(AD9833_FREQ0_ADDR | (uint16_t)((freqreg >> 14) & 0x3FFF));
}

void ad9833_init(void)
{
    /* Step 1: hard reset */
    ad9833_write_word(AD9833_B28 | AD9833_RESET);

    /* Step 2: load 1 kHz into FREQ0 */
    ad9833_write_word(AD9833_FREQ0_ADDR | (AD9833_FREQREG_1KHZ & 0x3FFF));
    ad9833_write_word(AD9833_FREQ0_ADDR | ((AD9833_FREQREG_1KHZ >> 14) & 0x3FFF));

    /* Step 3: PHASE0 = 0 (address 0b11 = 0xC000) */
    ad9833_write_word(0xC000);

    /* Step 4: clear RESET, enable square wave output (OPBITEN=1, MODE=0)
     *         B28=1 so future freq writes stay in 28-bit mode.           */
    ad9833_write_word(AD9833_B28 | AD9833_OPBITEN);
}
