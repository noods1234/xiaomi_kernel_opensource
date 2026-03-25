// SPDX-License-Identifier: GPL-2.0-only
// nd_v4l2_client.cpp

#include "nd_v4l2_client.h"

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cerrno>
#include <algorithm>

#include <linux/videodev2.h>
#include <sys/ioctl.h>

#define LOG_TAG "NdV4l2Client"
#include <log/log.h>

namespace oiw {

// ── Subdev discovery ─────────────────────────────────────────── //

static std::string findNdSubdev()
{
    // Scan /dev/v4l-subdev0..N for the one whose driver name is "nd_controller"
    DIR *dir = opendir("/dev");
    if (!dir) return {};

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "v4l-subdev", 10) != 0)
            continue;

        std::string path = std::string("/dev/") + ent->d_name;
        int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            if (strcmp(reinterpret_cast<const char *>(cap.driver), "nd_controller") == 0) {
                ::close(fd);
                closedir(dir);
                return path;
            }
        }
        ::close(fd);
    }
    closedir(dir);
    return {};
}

// ── Open / close ─────────────────────────────────────────────── //

bool NdV4l2Client::open()
{
    path_ = findNdSubdev();
    if (path_.empty()) {
        ALOGE("nd_controller subdev not found — is nd_controller.ko loaded?");
        return false;
    }

    fd_ = ::open(path_.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        ALOGE("open(%s): %s", path_.c_str(), strerror(errno));
        return false;
    }
    ALOGI("Opened nd_controller at %s (fd=%d)", path_.c_str(), fd_);
    return true;
}

NdV4l2Client::~NdV4l2Client()
{
    close();
}

void NdV4l2Client::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ── Single control helpers ────────────────────────────────────── //

bool NdV4l2Client::setCtrl(uint32_t cid, int32_t value)
{
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id    = cid;
    ctrl.value = value;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        ALOGE("VIDIOC_S_CTRL cid=0x%x val=%d: %s", cid, value, strerror(errno));
        return false;
    }
    return true;
}

bool NdV4l2Client::getCtrl(uint32_t cid, int32_t *value)
{
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = cid;
    if (ioctl(fd_, VIDIOC_G_CTRL, &ctrl) < 0) {
        ALOGE("VIDIOC_G_CTRL cid=0x%x: %s", cid, strerror(errno));
        return false;
    }
    *value = ctrl.value;
    return true;
}

// ── Write controls ────────────────────────────────────────────── //

bool NdV4l2Client::setNdTarget(int32_t nd_x100)
{
    nd_x100 = std::clamp(nd_x100, ND_MIN_X100, ND_MAX_X100);
    return setCtrl(CID_ND_TARGET, nd_x100);
}

bool NdV4l2Client::setMode(NdMode mode)
{
    return setCtrl(CID_ND_MODE, static_cast<int32_t>(mode));
}

bool NdV4l2Client::triggerCalibration()
{
    // BUTTON control: value is ignored by the driver
    return setCtrl(CID_ND_CAL_TRIG, 0);
}

// ── Batch read of all volatile status controls ────────────────── //

bool NdV4l2Client::getStatus(NdStatus *out)
{
    // Use G_EXT_CTRLS for a single round-trip to get all volatile fields
    struct v4l2_ext_control ctrls[4];
    memset(ctrls, 0, sizeof(ctrls));
    ctrls[0].id = CID_ND_ACTUAL;
    ctrls[1].id = CID_ND_TEMP;
    ctrls[2].id = CID_ND_SETTLED;
    ctrls[3].id = CID_ND_CCM_INDEX;

    struct v4l2_ext_controls ext;
    memset(&ext, 0, sizeof(ext));
    ext.which      = V4L2_CTRL_WHICH_CUR_VAL;
    ext.count      = 4;
    ext.controls   = ctrls;

    if (ioctl(fd_, VIDIOC_G_EXT_CTRLS, &ext) < 0) {
        ALOGE("VIDIOC_G_EXT_CTRLS: %s", strerror(errno));
        return false;
    }

    out->actual_x100    = ctrls[0].value;
    out->temp_decidegc  = ctrls[1].value;
    out->settled        = (ctrls[2].value != 0);
    out->ccm_index      = ctrls[3].value;
    return true;
}

} // namespace oiw
