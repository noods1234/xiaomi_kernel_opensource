#!/vendor/bin/sh
# cinema_on_apply.sh
# Called by cinema_on_apply service when persist.cinema.mode transitions to 1.
# Shrinks ZRAM from 4 GB to 2 GB to reduce Zstd compression pressure on the
# Little cluster during active RAW recording sessions.
#
# ZRAM resize requires: swapoff → disksize write → swapon.
# The disksize node only accepts writes while the device is inactive (swapped off).
#
# Race consideration: if cinema_off_apply is also running concurrently (rapid
# toggle), both scripts may attempt swapoff simultaneously.  swapoff is atomic
# at the kernel level — the second call returns EINVAL (device not a swap device)
# which is handled gracefully below.
# ------------------------------------------------------------------

ZRAM_DISKSIZE=2147483648   # 2 GB — minimal swap headroom during recording
ZRAM_DEV=/dev/zram0
ZRAM_SYS=/sys/block/zram0

# Only proceed if ZRAM is currently active as a swap device.
if ! grep -q zram0 /proc/swaps; then
    exit 0
fi

swapoff "$ZRAM_DEV"
if [ $? -ne 0 ]; then
    # swapoff failed — pages may be pinned (ION/DMA buffers in swap).
    # Log and exit; the 4 GB ZRAM stays active, which is acceptable.
    echo "cinema_on_apply: swapoff failed, keeping existing ZRAM size" > /dev/kmsg
    exit 0
fi

echo "$ZRAM_DISKSIZE" > "$ZRAM_SYS/disksize"
swapon "$ZRAM_DEV"

exit 0
