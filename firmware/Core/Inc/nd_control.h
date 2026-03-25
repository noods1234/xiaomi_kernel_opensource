/* SPDX-License-Identifier: GPL-2.0-only
 * nd_control.h — ND attenuation setpoint control
 *
 * Translates an ND target (0-255 encoded) to a MCP4131 wiper position
 * via the calibration LUT, manages the settle timer, and updates
 * the global nd_state_t.
 */
#pragma once
#include <stdint.h>

/* Set a new ND target. Applies immediately via LUT → wiper.
 * Starts the 15 ms settle timer; clears STATUS_SETTLED until settled. */
void nd_set_target(uint8_t nd_target_u8);

/* 10 ms periodic tick — call from control_task.
 * Decrements settle timer; sets STATUS_SETTLED when expired. */
void nd_tick_10ms(void);
