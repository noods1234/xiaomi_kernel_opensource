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
        echo 256   > "$dev/queue/nr_requests"         2>/dev/null
    fi
done

# ZRAM — activate only if not already an active swap device.
# Vendor init may have activated ZRAM before boot_completed; a duplicate
# swapon returns EBUSY and a duplicate disksize write is silently rejected
# by zram_drv, so guard both operations.
if ! grep -q zram0 /proc/swaps; then
    # BUG FIX 9: ZRAM block device node is /dev/zram0, not /dev/block/zram0.
    # /dev/block/zram0 does not exist on Android; swapon would fail with ENOENT.
    echo 8589934592 > /sys/block/zram0/disksize
    if ! swapon /dev/zram0; then
        echo "cinema_io_tune: swapon /dev/zram0 failed" > /dev/kmsg
    fi
fi

exit 0
