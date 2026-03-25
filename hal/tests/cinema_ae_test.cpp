// SPDX-License-Identifier: GPL-2.0-only
// cinema_ae_test.cpp — CinemaAe unit tests
//
// Run: atest nd_hal_tests  (or: ./nd_hal_tests on host)

#include "../cinema_ae.h"

#include <cassert>
#include <cstdio>
#include <cmath>

using namespace oiw;

// Helpers
static bool approx(float a, float b, float tol = 0.01f)
{
    return std::fabs(a - b) < tol;
}

// ── Test 1: dead-band — no ND change within ±0.15 EV ─────────── //
static void test_deadband()
{
    CinemaAe ae(30);
    ae.setTargetEv(14.0f);

    // Scene EV ≈ target_ev — ND should not change
    FrameMetadata f{};
    f.exposure_ns = CINEMA_SHUTTER_30FPS_NS;  // locked shutter
    f.iso         = CINEMA_ISO;               // base ISO
    f.nd_actual   = 3.0f;   // 3 stops ND already applied
    f.nd_settled  = true;

    // At ISO 64, 1/60 s: EV = log2(64/100) + log2(60) ≈ -0.644 + 5.907 = 5.26
    // scene_ev = 5.26 + 3.0 = 8.26 — far from 14.0, not in dead-band.
    // Force a scenario in dead-band by matching ev to target.
    // EV at ISO 64, 1/60 s = log2(0.64) + log2(60) ≈ 5.26.
    // scene_ev = current_ev + nd_actual = 5.26 + nd_actual.
    // For scene_ev = target_ev(14.0): nd_actual = 14.0 - 5.26 = 8.74 stops
    //   (beyond range, but tests the math)
    // Use nd_actual = 8.74 to put error in dead-band.
    f.nd_actual = 8.74f;

    AeOutput out = ae.update(f, 0);
    assert(!out.nd_changed);
    printf("PASS: test_deadband\n");
}

// ── Test 2: ND increases when scene is bright ─────────────────── //
static void test_nd_increases_when_overexposed()
{
    CinemaAe ae(30);
    ae.setTargetEv(14.0f);
    ae.reset();

    FrameMetadata f{};
    f.exposure_ns = CINEMA_SHUTTER_30FPS_NS;
    f.iso         = CINEMA_ISO;
    f.nd_actual   = ND_STOPS_MIN;   // 1.1 stops — very little ND
    f.nd_settled  = true;

    AeOutput out = ae.update(f, 0);
    // scene is much brighter than 14 EV target → ND should increase
    assert(out.nd_changed);
    assert(out.nd_target > ND_STOPS_MIN);
    printf("PASS: test_nd_increases_when_overexposed\n");
}

// ── Test 3: ND clamped at maximum ────────────────────────────── //
static void test_nd_clamped_at_max()
{
    CinemaAe ae(30);
    ae.setTargetEv(0.0f);   // unreachably dark target → ND maxes
    ae.reset();

    FrameMetadata f{};
    f.exposure_ns = CINEMA_SHUTTER_30FPS_NS;
    f.iso         = CINEMA_ISO;
    f.nd_actual   = ND_STOPS_MAX;
    f.nd_settled  = true;

    for (int i = 0; i < 10; i++)
        ae.update(f, 0);

    AeOutput out = ae.update(f, 0);
    assert(out.nd_target <= ND_STOPS_MAX);
    printf("PASS: test_nd_clamped_at_max\n");
}

// ── Test 4: CCM callback fires on settle ─────────────────────── //
static void test_ccm_callback_on_settle()
{
    CinemaAe ae(30);
    ae.setTargetEv(14.0f);

    bool ccm_fired = false;
    int  ccm_idx   = -1;
    ae.setCcmCallback([&](int idx) {
        ccm_fired = true;
        ccm_idx   = idx;
    });

    FrameMetadata f{};
    f.exposure_ns = CINEMA_SHUTTER_30FPS_NS;
    f.iso         = CINEMA_ISO;
    f.nd_actual   = 8.74f;   // in dead-band
    f.nd_settled  = false;

    ae.update(f, 5);   // not settled — callback must not fire
    assert(!ccm_fired);

    f.nd_settled = true;
    ae.update(f, 5);   // first settled frame — callback must fire
    assert(ccm_fired && ccm_idx == 5);

    ccm_fired = false;
    ae.update(f, 5);   // already was_settled — must not fire again
    assert(!ccm_fired);

    printf("PASS: test_ccm_callback_on_settle\n");
}

// ── Test 5: locked cinema shutter ────────────────────────────── //
static void test_locked_shutter()
{
    for (int fps : {24, 25, 30}) {
        CinemaAe ae(fps);
        ae.setTargetEv(14.0f);
        FrameMetadata f{};
        f.exposure_ns = 16'000'000LL;
        f.iso         = CINEMA_ISO;
        f.nd_actual   = 8.74f;
        f.nd_settled  = true;
        AeOutput out  = ae.update(f, 0);
        // Output shutter must be cinema angle regardless of input
        int64_t expected = (fps == 24) ? CINEMA_SHUTTER_24FPS_NS
                         : (fps == 25) ? CINEMA_SHUTTER_25FPS_NS
                                       : CINEMA_SHUTTER_30FPS_NS;
        assert(out.shutter_ns == expected);
        assert(out.iso == CINEMA_ISO);
    }
    printf("PASS: test_locked_shutter\n");
}

int main()
{
    test_deadband();
    test_nd_increases_when_overexposed();
    test_nd_clamped_at_max();
    test_ccm_callback_on_settle();
    test_locked_shutter();
    printf("All CinemaAe tests passed.\n");
    return 0;
}
