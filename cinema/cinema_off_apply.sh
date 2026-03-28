#!/vendor/bin/sh
# cinema_off_apply.sh
# Applied when persist.cinema.mode transitions to 0.
# Restores settings changed by cinema_on_apply.sh.
# Drop into device/xiaomi/aurora/rootdir/vendor/bin/
# ------------------------------------------------------------------

# Restore ZRAM to standard 8 GB.
if grep -q zram0 /proc/swaps; then
    swapoff /dev/zram0
    echo 8589934592 > /sys/block/zram0/disksize
    swapon /dev/zram0
fi

# Restore default swappiness.
echo 10 > /proc/sys/vm/swappiness

# Re-enable block I/O writeback throttle (default 75000 µs = 75 ms).
for dev in /sys/block/sd*; do
    [ -f "$dev/queue/wbt_lat_usec" ] && echo 75000 > "$dev/queue/wbt_lat_usec"
done

# Restore GPU idle behaviour.
echo 0 > /sys/class/kgsl/kgsl-3d0/force_no_nap     2>/dev/null
echo 80 > /sys/class/kgsl/kgsl-3d0/idle_timer       2>/dev/null

exit 0
