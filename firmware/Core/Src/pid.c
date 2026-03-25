/* SPDX-License-Identifier: GPL-2.0-only
 * pid.c — Discrete PID controller for MCU-side auto-ND (mode=2)
 *
 * Parameters from INTEGRATION_MAP.md:
 *   Dead-band:       ±0.15 EV (no output below threshold)
 *   kp (up):         0.55  (ND increasing — scene too bright)
 *   kp (down):       0.38  (ND decreasing — scene too dark)
 *   ki:              0.08
 *   kd:              0.04
 *   Integral clamp:  ±2.5 EV
 *   dt:              0.030 s (30 ms pid_task period)
 */

#include "pid.h"

#define KP_UP           0.55f
#define KP_DN           0.38f
#define KI              0.08f
#define KD              0.04f
#define DT              0.030f
#define DEAD_BAND       0.15f
#define INTEGRAL_CLAMP  2.5f

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ─────────────────────────────────────────────────────────────── */
float pid_update(pid_state_t *s, float error_ev)
{
    if (error_ev > -DEAD_BAND && error_ev < DEAD_BAND)
        return 0.0f;

    float kp = (error_ev > 0.0f) ? KP_UP : KP_DN;

    s->integral   = clampf(s->integral + error_ev * DT, -INTEGRAL_CLAMP, INTEGRAL_CLAMP);
    float deriv   = (error_ev - s->last_error) / DT;
    s->last_error = error_ev;

    return kp * error_ev + KI * s->integral + KD * deriv;
}

void pid_reset(pid_state_t *s)
{
    s->integral   = 0.0f;
    s->last_error = 0.0f;
}
