// SPDX-License-Identifier: GPL-2.0-only
// cinema_ae.cpp

#include "cinema_ae.h"

#include <cmath>
#include <algorithm>

#define LOG_TAG "CinemaAe"
#include <log/log.h>

namespace oiw {

namespace {

// PID gains (INTEGRATION_MAP.md §CINEMA_AE_ALGORITHM)
static constexpr float KP_UP          = 0.55f;   // ND increasing
static constexpr float KP_DN          = 0.38f;   // ND decreasing
static constexpr float KI             = 0.08f;
static constexpr float KD             = 0.04f;
static constexpr float DEAD_BAND_EV   = 0.15f;
static constexpr float INTEGRAL_CLAMP = 2.5f;

static float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

} // namespace

// ─────────────────────────────────────────────────────────────── //

CinemaAe::CinemaAe(int fps)
    : fps_(fps)
    , dt_(1.0f / fps)
    , target_ev_(14.0f)
{}

void CinemaAe::reset()
{
    integral_    = 0.0f;
    last_error_  = 0.0f;
    was_settled_ = false;
}

// ── EV computation ────────────────────────────────────────────── //
// EV = log2(N²/t) where t in seconds, N=1 (f/1 basis).
// Simplified for fixed aperture: EV = log2(iso/100) + log2(1e9/shutter_ns)
float CinemaAe::computeEv(int64_t shutter_ns, int32_t iso) const
{
    float t_sec = static_cast<float>(shutter_ns) * 1e-9f;
    return std::log2f(static_cast<float>(iso) / 100.0f)
         + std::log2f(1.0f / t_sec);
}

float CinemaAe::clampNd(float nd) const
{
    return clampf(nd, ND_STOPS_MIN, ND_STOPS_MAX);
}

// ── Per-frame update ──────────────────────────────────────────── //

AeOutput CinemaAe::update(const FrameMetadata &frame, int ccm_index)
{
    // ── Locked cinema shutter ──────────────────────────────── //
    int64_t shutter_ns;
    switch (fps_) {
    case 24: shutter_ns = CINEMA_SHUTTER_24FPS_NS; break;
    case 25: shutter_ns = CINEMA_SHUTTER_25FPS_NS; break;
    default: shutter_ns = CINEMA_SHUTTER_30FPS_NS; break;
    }

    // ── Current scene EV (using locked shutter + locked ISO 64) //
    float current_ev = computeEv(shutter_ns, CINEMA_ISO);

    // ── Error: positive means scene too bright → need more ND ── //
    // current_ev includes the ND attenuation already applied by
    // the sensor (ND darkens the scene, lowering raw EV at sensor).
    // Actual scene EV without ND = current_ev + nd_actual_stops.
    float scene_ev = current_ev + frame.nd_actual;
    float error_ev = target_ev_ - scene_ev;

    // ── Dead-band ─────────────────────────────────────────── //
    AeOutput out;
    out.shutter_ns  = shutter_ns;
    out.iso         = CINEMA_ISO;
    out.nd_target   = nd_target_;
    out.nd_changed  = false;

    if (std::fabs(error_ev) < DEAD_BAND_EV) {
        // Nothing to do — check settle and CCM injection
        if (frame.nd_settled && !was_settled_ && ccm_cb_) {
            ccm_cb_(ccm_index);
        }
        was_settled_ = frame.nd_settled;
        return out;
    }

    // ── PID ────────────────────────────────────────────────── //
    float kp = (error_ev > 0.0f) ? KP_UP : KP_DN;

    integral_  = clampf(integral_ + error_ev * dt_, -INTEGRAL_CLAMP, INTEGRAL_CLAMP);
    float deriv = (error_ev - last_error_) / dt_;
    last_error_ = error_ev;

    float delta_nd = kp * error_ev + KI * integral_ + KD * deriv;
    float nd_new   = clampNd(frame.nd_actual + delta_nd);

    if (nd_new != nd_target_) {
        nd_target_    = nd_new;
        out.nd_target = nd_new;
        out.nd_changed = true;
    }

    // ── ISO fallback (ND maxed, still overexposed) ─────────── //
    // Allow ISO > 64 only downward (to reduce brightness further).
    // The spec says "ISO fallback only if ND maxed and still overexposed",
    // but ISO increase (brightening) would require ND < min, which can't
    // happen — so ISO fallback is effectively only for overexposure edge.
    if (nd_new >= ND_STOPS_MAX && error_ev > DEAD_BAND_EV) {
        // Scene still too bright at max ND — raise ISO would make it
        // worse. We cannot reduce shutter below cinema angle.
        // Nothing left to do; log and hold.
        ALOGW("CinemaAe: max ND reached, scene still overexposed (error=%.2f EV)", error_ev);
    }

    // ── CCM injection on settle ────────────────────────────── //
    if (frame.nd_settled && !was_settled_ && ccm_cb_) {
        ccm_cb_(ccm_index);
    }
    was_settled_ = frame.nd_settled;

    return out;
}

} // namespace oiw
