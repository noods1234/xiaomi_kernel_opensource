#!/vendor/bin/sh
# cinema_on_apply.sh
# Applied when persist.cinema.mode transitions to 1.
# Handles operations too complex for simple sysfs writes in init.cinema.rc.
# Drop into device/xiaomi/aurora/rootdir/vendor/bin/
# ------------------------------------------------------------------

# Expand ZRAM to 12 GB for cinema RAW ring buffer headroom.
# Guard: zram cannot be resized while it has active swap.
if grep -q zram0 /proc/swaps; then
    swapoff /dev/zram0
    echo 12884901888 > /sys/block/zram0/disksize
    swapon /dev/zram0
fi

# Drop page cache to reclaim RAM for DMA capture buffers.
echo 3 > /proc/sys/vm/drop_caches

# Lower vm.swappiness further during recording — keep capture buffers hot.
echo 5 > /proc/sys/vm/swappiness

# Disable block writeback throttle on UFS LUNs during recording so
# large RAW writes are not artificially paced.
for dev in /sys/block/sd*; do
    [ -f "$dev/queue/wbt_lat_usec" ] && echo 0 > "$dev/queue/wbt_lat_usec"
done

# Raise GPU to no-nap state — prevent idle power collapse between frames.
echo 1 > /sys/class/kgsl/kgsl-3d0/force_no_nap     2>/dev/null
echo 0 > /sys/class/kgsl/kgsl-3d0/idle_timer        2>/dev/null

exit 0
