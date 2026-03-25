// SPDX-License-Identifier: GPL-2.0-only
// cinema_ae.h — Cinema AE state machine (Layer 3, HAL-side)
//
// ONE INCH WONDER — Xiaomi 13U/14U Camera HAL
// Document: LCHAL-001 Rev A  |  2026-03-24
//
// Exposure priority order (INTEGRATION_MAP.md CINEMA_AE_ALGORITHM):
//   1. Shutter — LOCKED at cinema 180° angle (1/(2×fps))
//   2. ISO     — LOCKED at base ISO 64
//   3. ND      — PRIMARY lever, 1.10–7.00 stops via PID
//   4. ISO fallback — only if ND maxed + scene still overexposed
//   5. Shutter fallback — last resort, underexpose only
//
// PID parameters:
//   dead-band  ±0.15 EV
//   kp  0.55 (ND increasing) / 0.38 (ND decreasing)
//   ki  0.08   kd  0.04   integral clamp ±2.5 EV
//   dt  1/fps (per-frame callback)

#pragma once

#include <cstdint>
#include <functional>

namespace oiw {

// ── Cinema shutter angles → exposure time in nanoseconds ──────── //
static constexpr int64_t CINEMA_SHUTTER_24FPS_NS = 20'833'333LL;  // 1/48 s
static constexpr int64_t CINEMA_SHUTTER_25FPS_NS = 20'000'000LL;  // 1/50 s
static constexpr int64_t CINEMA_SHUTTER_30FPS_NS = 16'666'667LL;  // 1/60 s
static constexpr int32_t CINEMA_ISO              = 64;

// ── ND range ─────────────────────────────────────────────────── //
static constexpr float ND_STOPS_MIN = 1.10f;
static constexpr float ND_STOPS_MAX = 7.00f;

// ── Per-frame capture result inputs ──────────────────────────── //
struct FrameMetadata {
    int64_t exposure_ns;    // ANDROID_SENSOR_EXPOSURE_TIME
    int32_t iso;            // ANDROID_SENSOR_SENSITIVITY
    float   nd_actual;      // from NdStatus.actual_x100 / 100.0f
    bool    nd_settled;     // from NdStatus.settled
};

// ── AE output per frame ───────────────────────────────────────── //
struct AeOutput {
    int64_t shutter_ns;     // always cinema angle (locked)
    int32_t iso;            // ISO 64 base, or fallback value
    float   nd_target;      // new ND target in stops (1.10–7.00)
    bool    nd_changed;     // true if nd_target differs from previous
};

// ── CCM injection callback ────────────────────────────────────── //
// Called by CinemaAe when nd_settled changes to true.
// Argument: ccm_index (0-60) from kernel.
using CcmCallback = std::function<void(int ccm_index)>;

// ──────────────────────────────────────────────────────────────── //

class CinemaAe {
public:
    explicit CinemaAe(int fps = 30);

    // Set target scene EV (default 14.0 = cinema daylight).
    void setTargetEv(float ev) { target_ev_ = ev; }

    // Register callback invoked when nd_settled becomes true.
    void setCcmCallback(CcmCallback cb) { ccm_cb_ = std::move(cb); }

    // Per-frame update. Returns AeOutput with updated exposure params.
    AeOutput update(const FrameMetadata &frame, int ccm_index);

    // Reset PID integrator (call on mode change or large scene jump).
    void reset();

private:
    float computeEv(int64_t shutter_ns, int32_t iso) const;
    float clampNd(float nd) const;

    int   fps_;
    float dt_;              // 1/fps
    float target_ev_;

    // PID state
    float integral_   = 0.0f;
    float last_error_ = 0.0f;

    // Tracking
    float nd_target_  = ND_STOPS_MIN;
    bool  was_settled_ = false;

    CcmCallback ccm_cb_;
};

} // namespace oiw
