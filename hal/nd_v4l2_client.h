// SPDX-License-Identifier: GPL-2.0-only
// nd_v4l2_client.h — V4L2 subdev control interface to nd_controller.ko
//
// ONE INCH WONDER — Xiaomi 13U/14U Camera HAL Layer 3
// Document: LCHAL-001 Rev A  |  2026-03-24
//
// Opens the nd_controller V4L2 subdev (found by scanning /dev/v4l-subdev*)
// and provides typed accessors for all seven custom controls.
//
// Custom CIDs (must match nd_controller.c):
//   V4L2_CID_USER_BASE + 0x1000  nd_target_stops_x100   RW
//   V4L2_CID_USER_BASE + 0x1001  nd_actual_stops_x100   RO volatile
//   V4L2_CID_USER_BASE + 0x1002  nd_mode                RW
//   V4L2_CID_USER_BASE + 0x1003  nd_cell_temp_decidegc  RO volatile
//   V4L2_CID_USER_BASE + 0x1004  nd_settled             RO volatile
//   V4L2_CID_USER_BASE + 0x1005  nd_ccm_index           RO volatile
//   V4L2_CID_USER_BASE + 0x1006  nd_cal_trigger         WO button

#pragma once

#include <cstdint>
#include <string>

namespace oiw {

// ── Control ID constants ─────────────────────────────────────── //
// Mirror of V4L2_CID_USER_BASE + offset (0x00980000 base on Android)
static constexpr uint32_t V4L2_CID_USER_BASE_ANDROID = 0x00980000U;

static constexpr uint32_t CID_ND_TARGET    = V4L2_CID_USER_BASE_ANDROID + 0x1000;
static constexpr uint32_t CID_ND_ACTUAL    = V4L2_CID_USER_BASE_ANDROID + 0x1001;
static constexpr uint32_t CID_ND_MODE      = V4L2_CID_USER_BASE_ANDROID + 0x1002;
static constexpr uint32_t CID_ND_TEMP      = V4L2_CID_USER_BASE_ANDROID + 0x1003;
static constexpr uint32_t CID_ND_SETTLED   = V4L2_CID_USER_BASE_ANDROID + 0x1004;
static constexpr uint32_t CID_ND_CCM_INDEX = V4L2_CID_USER_BASE_ANDROID + 0x1005;
static constexpr uint32_t CID_ND_CAL_TRIG  = V4L2_CID_USER_BASE_ANDROID + 0x1006;

// ── ND mode values ───────────────────────────────────────────── //
enum class NdMode : int32_t {
    Manual   = 0,
    AutoHal  = 1,   // HAL-side PID — this layer
    AutoMcu  = 2,   // MCU-side PID — STM32 drives itself
    Bypass   = 3,
};

// ── ND range (units: 0.01 stop) ──────────────────────────────── //
static constexpr int32_t ND_MIN_X100 = 110;   // ND 2.1
static constexpr int32_t ND_MAX_X100 = 700;   // ND 128

struct NdStatus {
    int32_t actual_x100;    // current ND attenuation × 100
    int32_t temp_decidegc;  // cell temperature × 10 °C
    bool    settled;        // true when ND has stabilised
    int32_t ccm_index;      // CCM table index (0-60)
};

// ────────────────────────────────────────────────────────────── //

class NdV4l2Client {
public:
    NdV4l2Client() = default;
    ~NdV4l2Client();

    // Scan /dev/v4l-subdev* for the nd_controller subdev and open it.
    // Returns true on success.
    bool open();

    // Close the subdev fd.
    void close();

    bool isOpen() const { return fd_ >= 0; }

    // ── Write controls ─────────────────────────────────────── //

    // Set commanded ND attenuation.  val clamped to [ND_MIN_X100, ND_MAX_X100].
    bool setNdTarget(int32_t nd_x100);

    // Set operating mode (NdMode enum).
    bool setMode(NdMode mode);

    // Pulse the calibration trigger (sends 0xCA to STM32).
    bool triggerCalibration();

    // ── Read controls ──────────────────────────────────────── //

    // Read all volatile status fields in a single G_EXT_CTRLS call.
    bool getStatus(NdStatus *out);

private:
    bool setCtrl(uint32_t cid, int32_t value);
    bool getCtrl(uint32_t cid, int32_t *value);

    int         fd_   = -1;
    std::string path_;
};

} // namespace oiw
