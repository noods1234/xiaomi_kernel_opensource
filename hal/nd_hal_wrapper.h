// SPDX-License-Identifier: GPL-2.0-only
// nd_hal_wrapper.h — Android Camera HAL3 wrapper (Layer 3)
//
// ONE INCH WONDER — Xiaomi 13U/14U Camera HAL
// Document: LCHAL-001 Rev A  |  2026-03-24
//
// Wraps the Qualcomm qcamera3 HAL3 implementation (DQ_2 approach):
// intercepts processCaptureRequest to inject ND setpoints and
// processCaptureResult to read AE metadata and drive CinemaAe.
//
// CCM injection path:
//   CinemaAe::ccm_callback → NdHalWrapper::injectCcm()
//     → ANDROID_COLOR_CORRECTION_MODE = TRANSFORM_MATRIX
//     → ANDROID_COLOR_CORRECTION_TRANSFORM = matrix[9] as Rational
//     → ANDROID_COLOR_CORRECTION_GAINS    = [R, Gr, Gb, B]
//
// Usage:
//   1. Load as a replacement camera.provider HAL (see Android.bp).
//   2. It dlopen()s the real qcamera HAL and wraps its function table.
//   3. Call NdHalWrapper::create() from camera_device_open().

#pragma once

#include "nd_v4l2_client.h"
#include "cinema_ae.h"

#include <hardware/camera3.h>
#include <utils/Mutex.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <system/camera_metadata.h>

namespace oiw {

// ── CCM entry from calibration JSON ──────────────────────────── //
struct CcmEntry {
    float nd_stops;
    std::array<float, 9> matrix3x3;   // row-major 3×3 CCM
    std::array<float, 4> gains_rggb;  // [R, Gr, Gb, B]
};

// ─────────────────────────────────────────────────────────────── //

class NdHalWrapper {
public:
    // Create wrapper around an already-opened real HAL device.
    static NdHalWrapper *create(camera3_device_t *real_dev, int fps = 30);
    ~NdHalWrapper();

    // ── camera3_device_ops replacements ─────────────────────── //
    int  initialize(const camera3_callback_ops_t *callback_ops);
    int  configureStreams(camera3_stream_configuration_t *stream_list);
    int  processCaptureRequest(camera3_capture_request_t *request);
    void dump(int fd);
    int  flush();

    // ── CCM table management ─────────────────────────────────── //
    // Load calibration JSON from path.  Must be called before streaming.
    bool loadCcmTable(const char *json_path);

    // ── Polling thread ────────────────────────────────────────── //
    // Reads nd_controller status at ~33 ms interval in a background thread.
    void startPolling();
    void stopPolling();

private:
    explicit NdHalWrapper(camera3_device_t *real_dev, int fps);

    // Trampoline: camera3_callback_ops_t must be first member so that a
    // pointer to it equals a pointer to this struct (standard layout).
    struct CallbackOpsWrapper {
        camera3_callback_ops_t ops;  // MUST remain first
        NdHalWrapper          *self;
    };

    // Result callback — intercepts each completed frame
    static void staticNotify(const camera3_callback_ops_t *ops,
                             const camera3_notify_msg_t   *msg);
    static void staticProcessCaptureResult(
                             const camera3_callback_ops_t    *ops,
                             const camera3_capture_result_t  *result);

    void onCaptureResult(const camera3_capture_result_t *result);
    void injectCcm(int ccm_index, camera_metadata_t *meta);
    void pollThread();

    // ── Per-request CCM injection helpers ────────────────────── //
    void buildCcmMetadata(const CcmEntry &entry, camera_metadata_t *meta);

    camera3_device_t            *real_dev_;
    const camera3_callback_ops_t *upstream_cb_ = nullptr;
    CallbackOpsWrapper           wrapper_cb_{};

    NdV4l2Client                 v4l2_;
    CinemaAe                     ae_;
    NdStatus                     nd_status_{};
    android::Mutex               nd_status_lock_;

    std::vector<CcmEntry>        ccm_table_;
    android::Mutex               ccm_lock_;

    // Pending CCM to inject on next request after settle
    bool       pending_ccm_     = false;
    int        pending_ccm_idx_ = 0;
    android::Mutex pending_ccm_lock_;

    // Owns the cloned metadata for the current request; freed on next request.
    struct CameraMetadataDeleter {
        void operator()(camera_metadata_t *m) const { free_camera_metadata(m); }
    };
    std::unique_ptr<camera_metadata_t, CameraMetadataDeleter> pending_meta_;

    std::thread  poll_thread_;
    std::atomic<bool> poll_running_{false};
};

} // namespace oiw
