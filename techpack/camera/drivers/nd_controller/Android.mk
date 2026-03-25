# Android.mk — techpack/camera/drivers/nd_controller
#
# ONE INCH WONDER — LC Variable ND Filter driver
# Xiaomi SM8550 (Kalama) vendor kernel module build
#
# Usage (device/xiaomi/thor/BoardConfig.mk):
#   BOARD_VENDOR_KERNEL_MODULES += \
#       $(KERNEL_MODULES_OUT)/nd_controller.ko
#
# The kernel module is installed to:
#   /vendor/lib/modules/nd_controller.ko
#
# modprobe loads it automatically on USB VID:PID 0483:5750 match via
# the MODULE_DEVICE_TABLE entry.

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE        := nd_controller.ko
LOCAL_MODULE_TAGS   := optional
LOCAL_MODULE_PATH   := $(KERNEL_MODULES_OUT)

# Kernel build outputs the .ko here after a techpack build
KBUILD_OPTIONS := \
    KBUILD_EXTRA_SYMBOLS=$(OUT)/obj/KERNEL_OBJ/Module.symvers \
    CONFIG_VIDEO_ND_CONTROLLER=m

$(LOCAL_MODULE): nd_controller.c nd_controller.h Kconfig Makefile
	@echo "Building nd_controller.ko"

include $(BUILD_PREBUILT)
