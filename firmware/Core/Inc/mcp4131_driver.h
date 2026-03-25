/* SPDX-License-Identifier: GPL-2.0-only
 * mcp4131_driver.h — MCP4131 10kΩ 128-step SPI digital potentiometer
 *
 * Wiper controls OPA548 Rin, setting LC drive amplitude.
 * wiper=0   → minimum gain (G≈1, safe on power-up)
 * wiper=127 → maximum gain (G≈10.1, ±15V output, clipped by ±18V rails)
 *
 * MCP4131 is volatile — wiper resets to mid-scale (~64) on power-up.
 * init() drives it to 0 before any LC cell is active.
 */
#pragma once
#include <stdint.h>

/* SPI command bytes */
#define MCP4131_CMD_WRITE   0x00U   /* write wiper register (addr=0) */
#define MCP4131_CMD_READ    0x0CU   /* read  wiper register           */

void     mcp4131_init(void);
void     mcp4131_set_wiper(uint8_t val);
uint8_t  mcp4131_get_wiper(void);
