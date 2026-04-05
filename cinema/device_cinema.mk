# device_cinema.mk — Cinema ROM product configuration for Xiaomi 14 Ultra (aurora)
#
# Include from device/xiaomi/aurora/device.mk (or aosp_aurora.mk):
#   $(call inherit-product, device/xiaomi/aurora/cinema/device_cinema.mk)
# -----------------------------------------------------------------------

# ---- Strip: system apps removed in cinema ROM ----
# AOSP/LineageOS note: there is no standard PRODUCT_PACKAGES_EX mechanism in
# AOSP or LineageOS device makefiles.  Package removal must be handled at the
# ROM overlay level.  For a LineageOS-based build, add the unwanted packages
# to the lineage_packages_remove.mk override, or use a privapp-permissions
# blocklist.  For reference, the apps below are candidates for removal:
#
#   YouTube, YouTubeMusicPrebuilt, GoogleMaps, Drive, Velvet,
#   CalculatorGooglePrebuilt, CalendarGooglePrebuilt, DeskClockGoogle,
#   GoogleFeedback, GoogleOneTimeInitializer, GoogleRestoreAgent, GoogleTTS,
#   MarkupGoogle, PixelWallpapers2023, WallpapersBReel2023, SafetyHubPrebuilt,
#   AndroidMigratePrebuilt, GoogleContacts, GoogleDialer, Phonesky,
#   NexusLauncherRelease

# ---- Keep: core system apps needed for cinema workflow ----
# (These are kept by default; listed here for documentation.)
# - Files (DocumentsUI) — for file management of captured footage
# - GalleryGo or Photos — for quick review
# - Settings — obviously
# - USB MTP — for file transfer

# ---- Add: cinema/pro-video packages ----
# NOTE: Third-party apps (MotionCam, FiLMiC Pro) cannot be listed in
# PRODUCT_PACKAGES without prebuilt APK definitions.  Install them via
# Magisk module or adb sideload after flashing.  See cinema/README.md
# for installation instructions.

# ---- Init fragments ----
PRODUCT_COPY_FILES += \
    device/xiaomi/aurora/cinema/init.cinema.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.cinema.rc \
    device/xiaomi/aurora/cinema/cinema_io_tune.sh:$(TARGET_COPY_OUT_VENDOR)/bin/cinema_io_tune.sh \
    device/xiaomi/aurora/cinema/cinema_on_apply.sh:$(TARGET_COPY_OUT_VENDOR)/bin/cinema_on_apply.sh \
    device/xiaomi/aurora/cinema/cinema_off_apply.sh:$(TARGET_COPY_OUT_VENDOR)/bin/cinema_off_apply.sh \
    device/xiaomi/aurora/cinema/ueventd.cinema.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.cinema.rc

# ---- System / vendor properties ----
PRODUCT_VENDOR_PROPERTIES += \
    persist.camera.HAL3.enabled=1 \
    persist.vendor.camera.HAL3.enabled=1 \
    persist.camera.raw.enable=1 \
    persist.camera.maxinflight=8 \
    persist.camera.zsl.mode=0 \
    persist.camera.eis.enable=0 \
    persist.camera.concurrent.streams=1 \
    persist.camera.raw10.enable=1 \
    persist.camera.res.fullres.enable=1 \
    persist.camera.video.4k120.enable=1 \
    persist.camera.video.8k30.enable=1 \
    persist.camera.dcg.enable=1 \
    persist.camera.unbinned_zoom.enable=1 \
    persist.camera.tnr.enable=0 \
    persist.camera.mfnr.enable=0 \
    persist.audio.fluence.voicecall=false \
    persist.audio.fluence.voicerecord=false \
    ro.adb.secure=0 \
    persist.cinema.mode=0

PRODUCT_SYSTEM_PROPERTIES += \
    ro.config.large_heap=true \
    ro.lmk.critical_upgrade=true \
    ro.lmk.upgrade_pressure=90 \
    ro.lmk.downgrade_pressure=85 \
    ro.config.media_vol_steps=100 \
    audio.offload.pcm.24bit.enable=true \
    audio.offload.buffer.size.kb=256 \
    media.stagefright.enable-record=true \
    debug.sf.enable_advanced_sf_phase_offset=1 \
    debug.sf.high_fps_early_gl_sf_phase_offset_ns=-2000000 \
    debug.sf.high_fps_early_sf_phase_offset_ns=-2000000 \
    persist.sys.usb.config=mtp,adb

# ---- Kernel ----
# Point the build at the cinema-optimised kernel image.
TARGET_KERNEL_CONFIG := vendor/pineapple_GKI.config \
                        vendor/aurora_GKI.config
TARGET_KERNEL_SOURCE := kernel/xiaomi/sm8650

# ---- Build flags ----
# Disable Bluetooth and WiFi scanning throttle during recording.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    wifi.supplicant_scan_interval=300 \
    bluetooth.core.le.vendor_capabilities.enabled=true

# Speed-profile DEX compilation for camera and media stack.
PRODUCT_DEX_PREOPT_DEFAULT_COMPILER_FILTER := speed-profile
PRODUCT_DEXPREOPT_SPEED_APPS += \
    CameraApp \
    MediaProvider \
    Launcher3QuickStep

# ---- Overlay: hide non-cinema quick-settings tiles ----
DEVICE_PACKAGE_OVERLAYS += device/xiaomi/aurora/cinema/overlay

# ---- Feature flags ----
# Declare Camera2 advanced features for apps that check features.xml.
PRODUCT_COPY_FILES += \
    device/xiaomi/aurora/cinema/cinema_features.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/cinema_features.xml
