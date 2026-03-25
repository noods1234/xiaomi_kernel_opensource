/* SPDX-License-Identifier: GPL-2.0-only
 * pid.h — Discrete PID controller (mode=2, MCU-side, 30 ms period)
 *
 * Asymmetric gains: kp_up=0.55 (ND increasing), kp_dn=0.38 (decreasing).
 * Dead-band: ±0.15 EV — no action below threshold.
 * Integral clamp: ±2.5 EV.
 */
#pragma once

typedef struct {
    float integral;
    float last_error;
} pid_state_t;

/* Compute ΔND (stops) from error_ev.  dt = 0.030 s (30 ms task period).
 * Returns 0.0f if |error_ev| < 0.15 (dead-band). */
float pid_update(pid_state_t *s, float error_ev);

/* Reset integrator and last_error to zero. */
void  pid_reset(pid_state_t *s);
