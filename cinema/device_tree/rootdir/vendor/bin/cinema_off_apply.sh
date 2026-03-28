#!/vendor/bin/sh
# cinema_off_apply.sh
# Called by cinema_off_apply service when persist.cinema.mode transitions to 0.
# Restores ZRAM to 4 GB after the recording session ends.
# ------------------------------------------------------------------

ZRAM_DISKSIZE=4294967296   # 4 GB — standard post-recording swap headroom
ZRAM_DEV=/dev/zram0
ZRAM_SYS=/sys/block/zram0

# Only proceed if ZRAM is currently active as a swap device.
if ! grep -q zram0 /proc/swaps; then
    # ZRAM is not active (cinema_on_apply may have failed).
    # Try to initialise it at the target size from scratch.
    echo "$ZRAM_DISKSIZE" > "$ZRAM_SYS/disksize" 2>/dev/null
    swapon "$ZRAM_DEV" 2>/dev/null
    exit 0
fi

swapoff "$ZRAM_DEV"
if [ $? -ne 0 ]; then
    # swapoff failed — pages may be in use.
    # Prefer to leave swap active at whatever size it currently is
    # rather than having no swap at all.
    echo "cinema_off_apply: swapoff failed, keeping existing ZRAM config" > /dev/kmsg
    exit 0
fi

echo "$ZRAM_DISKSIZE" > "$ZRAM_SYS/disksize"
swapon "$ZRAM_DEV"

exit 0
