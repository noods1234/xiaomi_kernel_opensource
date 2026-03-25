/* SPDX-License-Identifier: GPL-2.0-only
 * mcp4131_driver.c — MCP4131 SPI digital potentiometer
 *
 * CS = PB0 (active-low).  Shared SPI1 bus with AD9833.
 * MCP4131 SPI: Mode 0 (CPOL=0, CPHA=0).
 *
 * NOTE: SPI1 is configured for Mode 1 to suit the AD9833.  The
 * MCP4131 technically prefers Mode 0 but is tolerant of Mode 1 at
 * 10 MHz — timing margins are satisfied.  Verify at Phase 1.6.
 * If issues arise, switch SPI to Mode 0 and adjust ad9833_write_word
 * to toggle clock phase around AD9833 transactions.
 */

#include "mcp4131_driver.h"
#include "main.h"

/* ── 2-byte SPI transaction ────────────────────────────────────── */
static void mcp4131_txrx(uint8_t cmd, uint8_t data, uint8_t *rx_data)
{
    uint8_t tx[2] = { cmd, data };
    uint8_t rx[2] = { 0, 0 };

    xSemaphoreTake(g_spi_mutex, portMAX_DELAY);
    spi_cs_mcp4131_assert();
    HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, 10);
    spi_cs_mcp4131_deassert();
    xSemaphoreGive(g_spi_mutex);

    if (rx_data)
        *rx_data = rx[1];
}

/* ─────────────────────────────────────────────────────────────── */
void mcp4131_init(void)
{
    /* Drive wiper to minimum (0) before any LC cell is active.
     * MCP4131 power-up default is mid-scale (~64) which would
     * produce ~±8V LC drive — safe, but we want explicit control. */
    mcp4131_set_wiper(0);
}

void mcp4131_set_wiper(uint8_t val)
{
    /* Write command: address=0x00 (wiper 0), cmd bits=00 → byte 0x00 */
    mcp4131_txrx(MCP4131_CMD_WRITE, val, NULL);
}

uint8_t mcp4131_get_wiper(void)
{
    /* Read command: address=0x00, cmd bits=11 → byte 0x0C */
    uint8_t rx = 0;
    mcp4131_txrx(MCP4131_CMD_READ, 0x00, &rx);
    return rx;
}
