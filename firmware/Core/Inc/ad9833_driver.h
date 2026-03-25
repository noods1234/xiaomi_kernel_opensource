/* SPDX-License-Identifier: GPL-2.0-only
 * ad9833_driver.h — AD9833 DDS waveform generator driver
 *
 * Configured for 1 kHz square wave output via VOUT.
 * MCLK = 25 MHz (U7 oscillator).
 * FREQREG_1KHZ = round(1000 / 25e6 × 2^28) = 10737 = 0x29F1
 */
#pragma once
#include <stdint.h>

/* AD9833 control register bit positions */
#define AD9833_B28          (1U << 13)  /* 28-bit sequential freq write  */
#define AD9833_RESET        (1U << 8)   /* resets internal registers      */
#define AD9833_OPBITEN      (1U << 5)   /* output MSB of DAC (square wave)*/
#define AD9833_MODE         (1U << 1)   /* 0=sin/sq  1=triangle           */

/* FREQ0 register address prefix */
#define AD9833_FREQ0_ADDR   0x4000U     /* D15=1, D14=0 */

/* FREQREG value for 1 kHz at 25 MHz MCLK */
#define AD9833_FREQREG_1KHZ 10737U      /* 0x29F1 */

/* Initialise and start 1 kHz square wave output */
void ad9833_init(void);

/* Write a new frequency register value (28-bit) */
void ad9833_write_freq(uint32_t freqreg);
