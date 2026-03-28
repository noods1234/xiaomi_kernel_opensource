#!/vendor/bin/sh
# cinema_io_tune.sh
# Re-applies BFQ I/O tuning to all UFS block devices after boot and
# activates ZRAM swap idempotently (checks /proc/swaps first).
# Called once by init.cinema.rc via the cinema_io_tune service.
# Drop into device/xiaomi/aurora/rootdir/vendor/bin/
# ------------------------------------------------------------------

for dev in /sys/block/sd*; do
    if [ -d "$dev/queue" ]; then
        echo bfq   > "$dev/queue/scheduler"           2>/dev/null
        echo 1     > "$dev/queue/iosched/low_latency" 2>/dev/null
        echo 512   > "$dev/queue/read_ahead_kb"       2>/dev/null
        # 512 requests: UFS 4.0 NCQ saturates at high queue depth for
        # sequential RAW writes.  256 (BFQ default) leaves the controller
        # pipeline half-empty under sustained 4K RAW write load.
        echo 512   > "$dev/queue/nr_requests"         2>/dev/null
    fi
done

# ZRAM — activate only if not already an active swap device.
# Vendor init may have activated ZRAM before boot_completed; a duplicate
# swapon returns EBUSY and a duplicate disksize write is silently rejected
# by zram_drv, so guard both operations.
if ! grep -q zram0 /proc/swaps; then
    # ZRAM ceiling: 4 GB (4294967296 bytes).
    #
    # 8 GB was the previous value.  On a 12 GB device under RAW video load,
    # 8 GB of ZRAM means the kernel can swap up to ~3.2 GB of anonymous
    # pages (at ~2.5:1 Zstd compression).  The Zstd compression threads
    # compete directly with the Venus H.265/ProRes encoder for Little-cluster
    # CPU cycles, adding latency to DMA buffer recycling and increasing
    # frame-encode jitter.  4 GB provides enough headroom for background
    # system daemons without creating sustained compression pressure during
    # recording sessions.
    echo 4294967296 > /sys/block/zram0/disksize && swapon /dev/zram0
fi

exit 0
