// SPDX-License-Identifier: GPL-2.0-only
// nd_hal_wrapper.cpp

#include "nd_hal_wrapper.h"

#include <camera/CameraMetadata.h>
#include <system/camera_metadata.h>
#include <cmath>
#include <chrono>
#include <fstream>
#include <sstream>

#define LOG_TAG "NdHalWrapper"
#include <log/log.h>

// Minimal JSON parsing — replace with a proper library (nlohmann/json
// or libchrome JSON if available in the vendor partition) for production.
// We only need to read the calibration.json CCM table.
#include "ccm_json_parser.h"

namespace oiw {

// ── Static callback trampolines ───────────────────────────────── //

void NdHalWrapper::staticNotify(const camera3_callback_ops_t *ops,
                                const camera3_notify_msg_t   *msg)
{
    // ops points to CallbackOpsWrapper::ops (first member) — cast is safe.
    auto *w = reinterpret_cast<CallbackOpsWrapper *>(
        const_cast<camera3_callback_ops_t *>(ops));
    w->self->upstream_cb_->notify(w->self->upstream_cb_, msg);
}

void NdHalWrapper::staticProcessCaptureResult(
        const camera3_callback_ops_t   *ops,
        const camera3_capture_result_t *result)
{
    auto *w = reinterpret_cast<CallbackOpsWrapper *>(
        const_cast<camera3_callback_ops_t *>(ops));
    w->self->onCaptureResult(result);
}

// ── Construction / destruction ────────────────────────────────── //

NdHalWrapper::NdHalWrapper(camera3_device_t *real_dev, int fps)
    : real_dev_(real_dev)
    , ae_(fps)
{
    wrapper_cb_.ops.notify                = &staticNotify;
    wrapper_cb_.ops.process_capture_result = &staticProcessCaptureResult;
    wrapper_cb_.self = this;

    ae_.setCcmCallback([this](int idx) {
        android::Mutex::Autolock lock(pending_ccm_lock_);
        pending_ccm_     = true;
        pending_ccm_idx_ = idx;
    });
}

NdHalWrapper *NdHalWrapper::create(camera3_device_t *real_dev, int fps)
{
    auto *wrapper = new NdHalWrapper(real_dev, fps);
    if (!wrapper->v4l2_.open()) {
        ALOGE("Failed to open nd_controller V4L2 subdev");
        delete wrapper;
        return nullptr;
    }
    wrapper->v4l2_.setMode(NdMode::AutoHal);
    wrapper->startPolling();
    return wrapper;
}

NdHalWrapper::~NdHalWrapper()
{
    stopPolling();
    v4l2_.setMode(NdMode::Bypass);
    v4l2_.close();
}

// ── camera3_device_ops ────────────────────────────────────────── //

int NdHalWrapper::initialize(const camera3_callback_ops_t *cb)
{
    upstream_cb_ = cb;
    // Pass our intercept wrapper instead of the real cb
    return real_dev_->ops->initialize(real_dev_, &wrapper_cb_.ops);
}

int NdHalWrapper::configureStreams(camera3_stream_configuration_t *list)
{
    return real_dev_->ops->configure_streams(real_dev_, list);
}

int NdHalWrapper::processCaptureRequest(camera3_capture_request_t *req)
{
    // ── Inject pending CCM into the request settings ─────────── //
    {
        android::Mutex::Autolock lock(pending_ccm_lock_);
        if (pending_ccm_ && req->settings) {
            android::Mutex::Autolock cl(ccm_lock_);
            if (pending_ccm_idx_ < (int)ccm_table_.size()) {
                // Clone settings so we can modify them.  The clone is
                // owned by pending_meta_ and freed after the request
                // is forwarded to the real HAL.
                pending_meta_.reset(clone_camera_metadata(req->settings));
                buildCcmMetadata(ccm_table_[pending_ccm_idx_],
                                 pending_meta_.get());
                const_cast<camera3_capture_request_t *>(req)->settings =
                    pending_meta_.get();
            }
            pending_ccm_ = false;
        }
    }

    return real_dev_->ops->process_capture_request(real_dev_, req);
}

void NdHalWrapper::dump(int fd)
{
    real_dev_->ops->dump(real_dev_, fd);
}

int NdHalWrapper::flush()
{
    return real_dev_->ops->flush(real_dev_);
}

// ── Per-frame result handler ──────────────────────────────────── //

void NdHalWrapper::onCaptureResult(const camera3_capture_result_t *result)
{
    if (result->result) {
        // Extract AE metadata from the completed frame
        camera_metadata_ro_entry entry;

        FrameMetadata frame{};
        frame.nd_settled = false;
        frame.nd_actual  = ND_STOPS_MIN;

        // ANDROID_SENSOR_EXPOSURE_TIME
        if (find_camera_metadata_ro_entry(result->result,
                ANDROID_SENSOR_EXPOSURE_TIME, &entry) == 0 && entry.count > 0)
            frame.exposure_ns = entry.data.i64[0];

        // ANDROID_SENSOR_SENSITIVITY
        if (find_camera_metadata_ro_entry(result->result,
                ANDROID_SENSOR_SENSITIVITY, &entry) == 0 && entry.count > 0)
            frame.iso = entry.data.i32[0];

        // Fill nd_actual / nd_settled from last polled status
        {
            android::Mutex::Autolock lock(nd_status_lock_);
            frame.nd_actual  = nd_status_.actual_x100 / 100.0f;
            frame.nd_settled = nd_status_.settled;
        }

        int ccm_idx = 0;
        {
            android::Mutex::Autolock lock(nd_status_lock_);
            ccm_idx = nd_status_.ccm_index;
        }

        AeOutput ae_out = ae_.update(frame, ccm_idx);

        // Write new ND target to kernel subdev if changed
        if (ae_out.nd_changed) {
            int32_t nd_x100 = static_cast<int32_t>(ae_out.nd_target * 100.0f + 0.5f);
            v4l2_.setNdTarget(nd_x100);
        }
    }

    // Forward result upstream (to Android camera service)
    upstream_cb_->process_capture_result(upstream_cb_, result);
}

// ── CCM metadata injection ────────────────────────────────────── //

void NdHalWrapper::buildCcmMetadata(const CcmEntry &entry,
                                    camera_metadata_t *meta)
{
    // Helper: update existing tag or add it if absent.
    auto upsert = [&](uint32_t tag, const void *data, size_t count) {
        camera_metadata_entry_t e;
        if (find_camera_metadata_entry(meta, tag, &e) == 0)
            update_camera_metadata_entry(meta, e.index, data, count, nullptr);
        else
            add_camera_metadata_entry(meta, tag, data, count);
    };

    // COLOR_CORRECTION_MODE = TRANSFORM_MATRIX
    const uint8_t ccm_mode = ANDROID_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX;
    upsert(ANDROID_COLOR_CORRECTION_MODE, &ccm_mode, 1);

    // COLOR_CORRECTION_TRANSFORM = 3×3 matrix as camera_metadata_rational_t[9]
    camera_metadata_rational_t transform[9];
    for (int i = 0; i < 9; i++) {
        // Convert float to rational with denominator 1000
        transform[i].numerator   = static_cast<int32_t>(entry.matrix3x3[i] * 1000.0f);
        transform[i].denominator = 1000;
    }
    upsert(ANDROID_COLOR_CORRECTION_TRANSFORM, transform, 9);

    // COLOR_CORRECTION_GAINS = [R, Gr, Gb, B]
    upsert(ANDROID_COLOR_CORRECTION_GAINS, entry.gains_rggb.data(), 4);

    ALOGD("CCM injected for ND %.2f stops", entry.nd_stops);
}

void NdHalWrapper::injectCcm(int ccm_index, camera_metadata_t * /*meta*/)
{
    android::Mutex::Autolock lock(pending_ccm_lock_);
    pending_ccm_     = true;
    pending_ccm_idx_ = ccm_index;
}

// ── CCM table loader ──────────────────────────────────────────── //

bool NdHalWrapper::loadCcmTable(const char *json_path)
{
    android::Mutex::Autolock lock(ccm_lock_);
    ccm_table_ = parseCcmJson(json_path);
    if (ccm_table_.empty()) {
        ALOGE("loadCcmTable: failed to parse %s", json_path);
        return false;
    }
    ALOGI("Loaded %zu CCM entries from %s", ccm_table_.size(), json_path);
    return true;
}

// ── Background poll thread ─────────────────────────────────────── //

void NdHalWrapper::startPolling()
{
    poll_running_ = true;
    poll_thread_  = std::thread(&NdHalWrapper::pollThread, this);
}

void NdHalWrapper::stopPolling()
{
    poll_running_ = false;
    if (poll_thread_.joinable())
        poll_thread_.join();
}

void NdHalWrapper::pollThread()
{
    using namespace std::chrono_literals;
    while (poll_running_) {
        NdStatus status;
        if (v4l2_.getStatus(&status)) {
            android::Mutex::Autolock lock(nd_status_lock_);
            nd_status_ = status;
        }
        std::this_thread::sleep_for(33ms);
    }
}

} // namespace oiw
