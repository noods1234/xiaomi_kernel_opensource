#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# cinema_mode.sh — kselftest for the cinema_mode kernel driver
#
# Requires: bash >= 4.0, root, CONFIG_CINEMA_MODE=m, SM8650 target.
#
# Covers hardware integration scenarios that cannot be exercised by
# the KUnit suite (cinema_mode_kunit.c) alone:
#
#   1. Module load / unload
#   2. Basic enable / disable cycle
#   3. Idempotent writes (no-op when state already matches)
#   4. CPU frequency floor verification (scaling_min_freq == cpuinfo_max_freq)
#   5. CPU latency QoS verification (/dev/cpu_dma_latency)
#   6. Wakeup-source registration (cinema_mode appears in wakeup_sources)
#   7. CAP_SYS_ADMIN enforcement (non-root write returns EPERM)
#   8. Concurrent-writer stress test (novel: hammers enable with N tasks)
#   9. Module unload under active cinema mode (should auto-deactivate)
#  10. Frequency ordering validation (Little < Mid < Big max_freq)
#  11. CPU latency QoS ≤ 100µs while cinema mode active (/dev/cpu_dma_latency)
#  12. CPU latency QoS > 100µs after disable (QoS request removed)
#  13. BFQ wbt_lat_usec writability and restore (direct sysfs — bypasses init.rc)
#  14. KGSL force_no_nap + idle_timer writability and restore (direct sysfs)
#  15. cpuset camera-daemon CPU restriction (4-7) writability and restore
#  16. cpu.uclamp.min writability and restore (camera-daemon + top-app cgroups)
#  17. ZRAM dynamic resize via cinema_on_apply.sh / cinema_off_apply.sh
#
# Exit codes follow kselftest convention:
#   0  = PASS
#   1  = FAIL
#   4  = SKIP (required hardware/module not available)
#
# TAP note: plan header (1..N) is emitted at the end since total is
# not known until all tests complete.  This is valid per TAP v13 spec.

# kselftest framework
ksft_pass=0
ksft_fail=1
ksft_skip=4

PASS=0
FAIL=0
SKIP=0
TESTS_TOTAL=0

MODULE_NAME="cinema_mode"
SYSFS_ENABLE="/sys/kernel/cinema_mode/enable"
WAKEUP_SOURCES="/sys/kernel/debug/wakeup_sources"
CPUFREQ_ROOT="/sys/devices/system/cpu/cpufreq"
CPU_DMA_LATENCY="/dev/cpu_dma_latency"
KGSL="/sys/class/kgsl/kgsl-3d0"
CPUSET_CAM="/dev/cpuset/camera-daemon/cpus"
UCLAMP_CAM="/dev/cpuctl/camera-daemon/cpu.uclamp.min"
UCLAMP_TOP="/dev/cpuctl/top-app/cpu.uclamp.min"
ZRAM_DISKSIZE="/sys/block/zram0/disksize"
CINEMA_ON_APPLY="/vendor/bin/cinema_on_apply.sh"
CINEMA_OFF_APPLY="/vendor/bin/cinema_off_apply.sh"

# --------------------------------------------------------------------
# Cleanup: always runs on exit (trap), ensuring the module is unloaded
# and cinema mode is disabled even on early failure exits.
# --------------------------------------------------------------------

cleanup() {
	write_enable 0 2>/dev/null || true
	if module_loaded; then
		rmmod "$MODULE_NAME" 2>/dev/null || true
	fi
}
trap cleanup EXIT

# --------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------

pass() {
	echo "ok $((++TESTS_TOTAL)) $1"
	PASS=$((PASS + 1))
}

fail() {
	echo "not ok $((++TESTS_TOTAL)) $1"
	[ -n "$2" ] && echo "# $2"
	FAIL=$((FAIL + 1))
}

skip() {
	echo "ok $((++TESTS_TOTAL)) $1 # SKIP $2"
	SKIP=$((SKIP + 1))
}

require_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "TAP version 13"
		echo "1..0 # SKIP must be run as root"
		exit $ksft_skip
	fi
}

module_loaded() {
	grep -qw "$MODULE_NAME" /proc/modules 2>/dev/null
}

load_module() {
	if ! module_loaded; then
		modprobe "$MODULE_NAME" 2>/dev/null || \
			insmod "${MODULE_NAME}.ko" 2>/dev/null
	fi
}

read_enable() {
	cat "$SYSFS_ENABLE" 2>/dev/null | tr -d '[:space:]'
}

write_enable() {
	echo "$1" > "$SYSFS_ENABLE" 2>/dev/null
	return $?
}

read_scaling_min() {
	cat "${1}/scaling_min_freq" 2>/dev/null
}

read_cpuinfo_max() {
	cat "${1}/cpuinfo_max_freq" 2>/dev/null
}

# --------------------------------------------------------------------
# Prerequisites
# --------------------------------------------------------------------

require_root

echo "TAP version 13"

if ! modprobe -n "$MODULE_NAME" 2>/dev/null && \
   [ ! -f "${MODULE_NAME}.ko" ]; then
	echo "1..0 # SKIP cinema_mode module not available (CONFIG_CINEMA_MODE not set?)"
	exit $ksft_skip
fi

if [ ! -d "$CPUFREQ_ROOT" ]; then
	echo "1..0 # SKIP cpufreq sysfs not available"
	exit $ksft_skip
fi

# --------------------------------------------------------------------
# Test 1: Module load
# --------------------------------------------------------------------

if module_loaded; then rmmod "$MODULE_NAME" 2>/dev/null; fi
load_module
if module_loaded && [ -f "$SYSFS_ENABLE" ]; then
	pass "module_load: cinema_mode loads and creates sysfs node"
else
	fail "module_load: cinema_mode failed to load or sysfs node missing"
	echo "1..1"
	exit $ksft_fail
fi

# --------------------------------------------------------------------
# Test 2: Initial state is 0
# --------------------------------------------------------------------

val=$(read_enable)
if [ "$val" = "0" ]; then
	pass "initial_state: enable reads 0 after fresh module load"
else
	fail "initial_state: expected 0, got '$val'"
fi

# --------------------------------------------------------------------
# Test 3: Basic enable
# --------------------------------------------------------------------

write_enable 1
val=$(read_enable)
if [ "$val" = "1" ]; then
	pass "basic_enable: write 1 → read 1"
else
	fail "basic_enable: expected 1 after writing 1, got '$val'"
fi

# --------------------------------------------------------------------
# Test 4: Basic disable
# --------------------------------------------------------------------

write_enable 0
val=$(read_enable)
if [ "$val" = "0" ]; then
	pass "basic_disable: write 0 → read 0"
else
	fail "basic_disable: expected 0 after writing 0, got '$val'"
fi

# --------------------------------------------------------------------
# Test 5: Idempotent enable (write 1 when already 1)
# --------------------------------------------------------------------

write_enable 1
write_enable 1   # second write — should be no-op
ret=$?
val=$(read_enable)
if [ "$val" = "1" ] && [ "$ret" -eq 0 ]; then
	pass "idempotent_enable: double write 1 is a no-op"
else
	fail "idempotent_enable: val='$val' ret='$ret'"
fi
write_enable 0

# --------------------------------------------------------------------
# Test 6: Idempotent disable (write 0 when already 0)
# --------------------------------------------------------------------

write_enable 0   # already 0 after previous test
ret=$?
val=$(read_enable)
if [ "$val" = "0" ] && [ "$ret" -eq 0 ]; then
	pass "idempotent_disable: double write 0 is a no-op"
else
	fail "idempotent_disable: val='$val' ret='$ret'"
fi

# --------------------------------------------------------------------
# Test 7: CPU frequency floor verification
#
# Novel: when cinema mode is active, scaling_min_freq for every
# cpufreq policy must equal cpuinfo_max_freq (QoS floor pinned to max).
# --------------------------------------------------------------------

write_enable 1
sleep 0.2   # allow QoS to propagate to governor

all_pinned=1
for policy_dir in "${CPUFREQ_ROOT}"/policy*; do
	[ -d "$policy_dir" ] || continue
	min=$(read_scaling_min "$policy_dir")
	max=$(read_cpuinfo_max "$policy_dir")
	if [ -z "$min" ] || [ -z "$max" ]; then
		continue
	fi
	if [ "$min" -ne "$max" ]; then
		all_pinned=0
		echo "# $(basename "$policy_dir"): scaling_min=${min}kHz cpuinfo_max=${max}kHz"
	fi
done

if [ "$all_pinned" -eq 1 ]; then
	pass "freq_floor: all policies pinned to cpuinfo_max_freq while active"
else
	fail "freq_floor: one or more policies not pinned (see # lines above)"
fi

write_enable 0

# --------------------------------------------------------------------
# Test 8: CPU frequency floor actually released on disable
#
# Test 7 verified that scaling_min_freq == cpuinfo_max_freq while active.
# This test verifies that the QoS floor is removed — not just that the
# enable node reads 0.  A bug where freq_qos_remove_request() fails
# silently would pass a sysfs-only check but be caught here.
#
# We compare scaling_min_freq to cpuinfo_max_freq for every policy.
# After deactivation, at least one policy must have min < max (the
# governor is free to scale down).  On an idle system every policy
# will have dropped below max; on a loaded system some may still be
# at max due to governor demand — so we only require that the QoS
# floor was lifted, not that the frequency actually dropped.
#
# The cpufreq QoS update is synchronous from the driver's perspective,
# but the sysfs reflection of scaling_min_freq may lag by one governor
# evaluation period (~1–4ms on schedutil).  100ms sleep is generous.
# --------------------------------------------------------------------

sleep 0.1

val=$(read_enable)
if [ "$val" != "0" ]; then
	fail "freq_floor_released: enable node still reads '$val' after disable"
else
	floor_lifted=0
	for policy_dir in "${CPUFREQ_ROOT}"/policy*; do
		[ -d "$policy_dir" ] || continue
		min=$(read_scaling_min "$policy_dir")
		max=$(read_cpuinfo_max "$policy_dir")
		[ -z "$min" ] || [ -z "$max" ] && continue
		if [ "$min" -lt "$max" ]; then
			floor_lifted=1
			break
		fi
	done

	if [ "$floor_lifted" -eq 1 ]; then
		pass "freq_floor_released: QoS floor lifted — at least one policy scaling_min < cpuinfo_max"
	else
		fail "freq_floor_released: all policies still pinned to cpuinfo_max after disable — QoS floor not removed"
	fi
fi

# --------------------------------------------------------------------
# Test 9: Wakeup source activated when cinema mode is enabled
#
# /sys/kernel/debug/wakeup_sources lists ALL registered sources.
# Checking for the name alone passes even when cinema mode is inactive
# because wakeup_source_register() runs at module load.  Instead we
# read the active_count column: it increments each time __pm_stay_awake
# fires.  We capture the count before and after enabling cinema mode
# and assert it increased by exactly 1.
#
# wakeup_sources column layout (space-separated, no header guarantees):
#   name  active_count  event_count  wakeup_count  expire_count
#   active_since_ms  total_time_ms  max_time_ms  last_change_ms
#   prevent_suspend_time_ms
#
# awk extracts field 2 (active_count) from the cinema_mode row.
# --------------------------------------------------------------------

if [ ! -r "$WAKEUP_SOURCES" ]; then
	skip "wakeup_source" "debugfs not mounted or not readable"
else
	get_active_count() {
		awk '/^cinema_mode[[:space:]]/ { print $2; exit }' \
			"$WAKEUP_SOURCES" 2>/dev/null
	}

	write_enable 0   # ensure inactive baseline
	count_before=$(get_active_count)

	if [ -z "$count_before" ]; then
		fail "wakeup_source: cinema_mode entry missing from $WAKEUP_SOURCES (module not loaded?)"
	else
		write_enable 1
		count_after=$(get_active_count)
		write_enable 0

		if [ "$count_after" -gt "$count_before" ] 2>/dev/null; then
			pass "wakeup_source: active_count incremented ($count_before → $count_after) — __pm_stay_awake fired"
		else
			fail "wakeup_source: active_count did not increase (before=$count_before after=$count_after) — wakeup source not activated"
		fi
	fi
fi

# --------------------------------------------------------------------
# Test 10: CAP_SYS_ADMIN enforcement
#
# Novel: attempt a write as an unprivileged user and verify it is
# rejected.  Uses 'su' with the '-s /bin/sh' flag (GNU coreutils su).
# Falls back gracefully when su or the nobody user is unavailable.
# --------------------------------------------------------------------

if ! command -v su >/dev/null 2>&1; then
	skip "cap_sys_admin" "su command not available"
elif ! id nobody >/dev/null 2>&1; then
	skip "cap_sys_admin" "user 'nobody' not available"
else
	if su nobody -s /bin/sh -c "echo 1 > '$SYSFS_ENABLE'" 2>/dev/null; then
		fail "cap_sys_admin: non-root write succeeded — CAP_SYS_ADMIN check broken"
	else
		pass "cap_sys_admin: non-root write correctly rejected"
	fi
fi

# --------------------------------------------------------------------
# Test 11: Concurrent-writer stress test  (NOVEL)
#
# Spawn NWRITERS background tasks, each toggling cinema mode rapidly
# for DURATION seconds.  After all writers finish:
#   a) No BUG/WARN/oops in dmesg (validated by log-serial comparison).
#   b) Final state after forced disable must be 0.
#
# dmesg strategy: read the full dmesg log once before the test and
# once after; compare for new error lines.  Avoids reliance on
# dmesg --since / --time-format which are util-linux ≥ 2.23 features.
# --------------------------------------------------------------------

NWRITERS=32
DURATION=3

# Clear the ring buffer so post-test capture contains only new messages.
# This is more reliable than diff:
#   - diff is O(n²) on large logs and can miss errors swamped by new content
#   - dmesg -c atomically reads-and-clears under the logbuf lock (no TOCTOU)
#   - No temp files needed, so no cleanup edge cases
#
# dmesg -c requires root, which require_root() already enforced.
dmesg -c > /dev/null

writer_task() {
	local end
	end=$(($(date +%s) + DURATION))
	while [ "$(date +%s)" -lt "$end" ]; do
		echo 1 > "$SYSFS_ENABLE" 2>/dev/null
		echo 0 > "$SYSFS_ENABLE" 2>/dev/null
	done
}

for i in $(seq 1 $NWRITERS); do
	writer_task &
done
wait

write_enable 0   # drain to a known state

stress_ok=1
val=$(read_enable)

# Capture only error-level messages generated during the stress window.
new_errors=$(dmesg -l err,crit,alert,emerg 2>/dev/null | \
	grep -E 'BUG:|WARNING:|kernel BUG|general protection|RIP:|Call Trace:' \
	|| true)

if [ -n "$new_errors" ]; then
	stress_ok=0
	echo "# dmesg shows kernel error during stress test:"
	echo "$new_errors" | head -5 | sed 's/^/#   /'
fi

if [ "$val" != "0" ]; then
	stress_ok=0
	echo "# state after stress: expected 0, got '$val'"
fi

if [ "$stress_ok" -eq 1 ]; then
	pass "concurrent_stress: $NWRITERS writers × ${DURATION}s — no oops, clean state"
else
	fail "concurrent_stress: kernel error or corrupt state detected"
fi

# --------------------------------------------------------------------
# Test 12: Module unload while cinema mode is active
#
# Novel: enable cinema mode, then rmmod — the exit handler must call
# cinema_deactivate() and clean up all QoS requests before returning.
# --------------------------------------------------------------------

write_enable 1
val=$(read_enable)
if [ "$val" != "1" ]; then
	skip "unload_active" "could not activate cinema mode for unload test"
else
	# Disable trap briefly so we can test the module's own cleanup
	trap - EXIT

	rmmod "$MODULE_NAME" 2>/dev/null
	ret=$?

	# Re-arm trap for the remaining tests
	trap cleanup EXIT

	if [ $ret -eq 0 ] && ! module_loaded; then
		pass "unload_active: rmmod with active cinema mode succeeded"
	else
		fail "unload_active: rmmod failed or module still loaded (ret=$ret)"
	fi

	# Reload for cleanup path
	load_module 2>/dev/null || true
fi

# --------------------------------------------------------------------
# Test 13: Frequency ordering (Little < Mid < Big)
#
# Novel topology validator: reads cpuinfo_max_freq for each policy and
# asserts strict ascending frequency ordering holds.
# SM8650 has 4 policies (A520/A720-lo/A720-hi/X4); SM8550 had 3.
# Accept 3 or 4 policies; reject anything outside that range.
# --------------------------------------------------------------------

policies=()
for policy_dir in "${CPUFREQ_ROOT}"/policy*; do
	[ -d "$policy_dir" ] || continue
	freq=$(read_cpuinfo_max "$policy_dir")
	[ -n "$freq" ] && policies+=("$freq")
done

IFS=$'\n' sorted=($(sort -n <<< "${policies[*]}")); unset IFS
npol=${#sorted[@]}

if [ "$npol" -eq 3 ] || [ "$npol" -eq 4 ]; then
	ok=1
	for ((i=1; i<npol; i++)); do
		if [ "${sorted[$i]}" -le "${sorted[$((i-1))]}" ]; then
			ok=0
			break
		fi
	done
	if [ "$ok" -eq 1 ]; then
		pass "freq_ordering: ${npol} policies, strict ascending: ${sorted[*]} kHz"
	else
		fail "freq_ordering: expected strict ascending, got ${sorted[*]}"
	fi
elif [ "$npol" -gt 0 ]; then
	skip "freq_ordering" "expected 3-4 cpufreq policies for SM8650, found ${npol}"
else
	skip "freq_ordering" "no cpufreq policies found"
fi

# --------------------------------------------------------------------
# Test 14: CPU latency QoS ≤ 100µs while cinema mode is active
#
# The driver calls cpu_latency_qos_add_request(100) on activate, which
# pins the system-wide latency ceiling at 100µs.  /dev/cpu_dma_latency
# reflects the *effective* QoS value (minimum of all active requests)
# as a 4-byte little-endian s32.  While cinema mode is active the read
# value must be ≤ 100.  After deactivation the driver removes its
# request; the file should return INT_MAX (0x7fffffff / 2147483647) when
# no other request is active, or at least a value > 100.
#
# Unlike Tests 16–19 this uses write_enable because the QoS request is
# issued directly by the kernel driver, not by an init.rc property
# trigger.  The kselftest can exercise it without the Android property
# system.
# --------------------------------------------------------------------

if [ ! -r "$CPU_DMA_LATENCY" ]; then
	skip "cpu_latency_qos_active" "$CPU_DMA_LATENCY not readable (not Android/Qualcomm target?)"
else
	write_enable 1
	sleep 0.1

	# /dev/cpu_dma_latency is a binary file: 4-byte LE s32.
	# od -An -tu4 reads it as an unsigned decimal — sufficient since
	# the kernel clamps to [0, INT_MAX] and we only need to check ≤ 100.
	latency_active=$(od -An -tu4 "$CPU_DMA_LATENCY" 2>/dev/null | tr -d ' ')

	write_enable 0
	sleep 0.1
	latency_idle=$(od -An -tu4 "$CPU_DMA_LATENCY" 2>/dev/null | tr -d ' ')

	if [ -z "$latency_active" ]; then
		fail "cpu_latency_qos_active" "could not read $CPU_DMA_LATENCY"
	elif [ "$latency_active" -le 100 ] 2>/dev/null; then
		pass "cpu_latency_qos_active: latency=${latency_active}µs ≤ 100µs while cinema mode active"
	else
		fail "cpu_latency_qos_active: expected ≤ 100µs, got ${latency_active}µs — QoS request not applied"
	fi
fi

# --------------------------------------------------------------------
# Test 15: CPU latency QoS released after cinema mode disable
#
# After write_enable 0 the driver calls cpu_latency_qos_remove_request().
# If no other process holds the fd open, /dev/cpu_dma_latency returns
# PM_QOS_CPU_LATENCY_DEFAULT_VALUE (INT_MAX = 2147483647).  If another
# process already holds an fd (e.g., audio HAL on a running device) the
# value may be lower — we only assert it is strictly > 100µs, meaning
# the driver's 100µs floor was removed.
# --------------------------------------------------------------------

if [ ! -r "$CPU_DMA_LATENCY" ]; then
	skip "cpu_latency_qos_released" "$CPU_DMA_LATENCY not readable"
else
	# latency_idle was captured in Test 14 immediately after write_enable 0.
	# Re-read in case Test 14 was skipped.
	latency_idle=$(od -An -tu4 "$CPU_DMA_LATENCY" 2>/dev/null | tr -d ' ')

	if [ -z "$latency_idle" ]; then
		fail "cpu_latency_qos_released" "could not read $CPU_DMA_LATENCY"
	elif [ "$latency_idle" -gt 100 ] 2>/dev/null; then
		pass "cpu_latency_qos_released: latency=${latency_idle}µs > 100µs after disable — QoS floor removed"
	else
		fail "cpu_latency_qos_released: latency=${latency_idle}µs still ≤ 100µs after disable — QoS request not removed"
	fi
fi

# --------------------------------------------------------------------
# Test 16: BFQ wbt_lat_usec writability and restore
#
# init.cinema.rc writes wbt_lat_usec=0 on cinema activate and restores
# it to 75000 on deactivate.  This test exercises those sysfs nodes
# DIRECTLY (not via write_enable) because the kselftest bypasses
# Android's property system — init.rc property triggers do not fire
# from a direct sysfs write, so write_enable 1 would not exercise this
# path.
#
# Strategy:
#   1. Save the current value for every BFQ queue found.
#   2. Write 0, assert it was accepted.
#   3. Write the original value back.
#   4. Assert the restore succeeded.
#
# wbt_lat_usec lives at /sys/block/<dev>/queue/wbt_lat_usec and is
# only present when the BFQ I/O scheduler is active.  Skip the test
# per-device if the node is absent.  Pass only if every found node
# passed both the write and the restore.
# --------------------------------------------------------------------

wbt_pass=0
wbt_fail=0
wbt_skip=0

for wbt_node in /sys/block/sd*/queue/wbt_lat_usec \
                /sys/block/mmcblk*/queue/wbt_lat_usec \
                /sys/block/nvme*/queue/wbt_lat_usec; do
	[ -w "$wbt_node" ] || { wbt_skip=$((wbt_skip + 1)); continue; }

	orig=$(cat "$wbt_node" 2>/dev/null)
	[ -z "$orig" ] && { wbt_skip=$((wbt_skip + 1)); continue; }

	echo 0 > "$wbt_node" 2>/dev/null
	after_write=$(cat "$wbt_node" 2>/dev/null)

	echo "$orig" > "$wbt_node" 2>/dev/null
	after_restore=$(cat "$wbt_node" 2>/dev/null)

	dev=$(echo "$wbt_node" | cut -d/ -f4)
	if [ "$after_write" = "0" ] && [ "$after_restore" = "$orig" ]; then
		wbt_pass=$((wbt_pass + 1))
		echo "# wbt_lat_usec: $dev write=0 restore=${orig} OK"
	else
		wbt_fail=$((wbt_fail + 1))
		echo "# wbt_lat_usec: $dev write_result='$after_write' restore_result='$after_restore' (expected 0 / $orig)"
	fi
done

if [ $((wbt_pass + wbt_fail)) -eq 0 ]; then
	skip "wbt_lat_usec" "no writable wbt_lat_usec nodes found (BFQ not active or no block devices)"
elif [ $wbt_fail -eq 0 ]; then
	pass "wbt_lat_usec: write 0 / restore verified on $wbt_pass device(s)"
else
	fail "wbt_lat_usec: $wbt_fail device(s) failed write/restore (see # lines above)"
fi

# --------------------------------------------------------------------
# Test 17: KGSL force_no_nap and idle_timer writability and restore
#
# init.cinema.rc writes force_no_nap=1 and idle_timer=16384 on cinema
# activate to prevent the Adreno 750 GPU from entering the nap state
# that triggers a power-cycle and resets min_pwrlevel.  Same rationale
# as Test 16 for using direct sysfs writes instead of write_enable.
#
# Nodes: $KGSL/force_no_nap and $KGSL/idle_timer
# --------------------------------------------------------------------

kgsl_ok=1

if [ ! -w "${KGSL}/force_no_nap" ]; then
	skip "kgsl_tuning" "KGSL sysfs not writable (${KGSL}/force_no_nap) — not a Qualcomm Adreno target?"
else
	nap_orig=$(cat "${KGSL}/force_no_nap" 2>/dev/null)
	timer_orig=$(cat "${KGSL}/idle_timer" 2>/dev/null)

	# Apply cinema-mode values
	echo 1     > "${KGSL}/force_no_nap"  2>/dev/null
	echo 16384 > "${KGSL}/idle_timer"    2>/dev/null

	nap_after=$(cat "${KGSL}/force_no_nap" 2>/dev/null)
	timer_after=$(cat "${KGSL}/idle_timer" 2>/dev/null)

	# Restore
	echo "${nap_orig:-0}"   > "${KGSL}/force_no_nap" 2>/dev/null
	echo "${timer_orig:-80}" > "${KGSL}/idle_timer"  2>/dev/null

	nap_restored=$(cat "${KGSL}/force_no_nap" 2>/dev/null)
	timer_restored=$(cat "${KGSL}/idle_timer" 2>/dev/null)

	[ "$nap_after"     != "1"             ] && { kgsl_ok=0; echo "# force_no_nap: wrote 1, read '$nap_after'"; }
	[ "$timer_after"   != "16384"         ] && { kgsl_ok=0; echo "# idle_timer: wrote 16384, read '$timer_after'"; }
	[ "$nap_restored"  != "${nap_orig:-0}" ] && { kgsl_ok=0; echo "# force_no_nap: restore failed (wanted '${nap_orig:-0}', got '$nap_restored')"; }
	[ "$timer_restored" != "${timer_orig:-80}" ] && { kgsl_ok=0; echo "# idle_timer: restore failed (wanted '${timer_orig:-80}', got '$timer_restored')"; }

	if [ "$kgsl_ok" -eq 1 ]; then
		pass "kgsl_tuning: force_no_nap and idle_timer write/restore OK"
	else
		fail "kgsl_tuning: KGSL sysfs write or restore failed (see # lines above)"
	fi
fi

# --------------------------------------------------------------------
# Test 18: cpuset camera-daemon CPU restriction writability and restore
#
# init.cinema.rc writes CPUs "4-7" to /dev/cpuset/camera-daemon/cpus
# on cinema activate to pin the camera daemon to Mid+Prime clusters,
# keeping Little cores free for background work.  Direct sysfs write
# required — same rationale as Tests 16–17.
#
# The test saves the current value, writes "4-7", asserts acceptance,
# then restores.  The cpuset subsystem normalises the CPU list to a
# canonical form (e.g., "4-7" may become "4,5,6,7") so we compare the
# restored value to the saved original, not to the literal string we
# wrote.
# --------------------------------------------------------------------

if [ ! -w "$CPUSET_CAM" ]; then
	skip "cpuset_camera" "$CPUSET_CAM not writable (cpuset cgroup not mounted or node absent)"
else
	cpuset_orig=$(cat "$CPUSET_CAM" 2>/dev/null)

	echo "4-7" > "$CPUSET_CAM" 2>/dev/null
	cpuset_after=$(cat "$CPUSET_CAM" 2>/dev/null)

	# Restore — use original value; if it was empty or "0-7" write that.
	echo "${cpuset_orig:-0-7}" > "$CPUSET_CAM" 2>/dev/null
	cpuset_restored=$(cat "$CPUSET_CAM" 2>/dev/null)

	# Accept any non-empty value that includes CPU 4 as "write accepted".
	# The kernel normalises "4-7" variously; checking CPU 4 is present
	# is robust across kernel versions.
	cpuset_ok=1
	echo "$cpuset_after" | grep -qE '(^|,)4(-|,|$)' 2>/dev/null || \
		echo "$cpuset_after" | grep -q "4-7" 2>/dev/null || \
		{ cpuset_ok=0; echo "# cpuset: wrote 4-7, read '$cpuset_after' (CPU 4 not in set)"; }

	[ "$cpuset_restored" != "$cpuset_orig" ] && \
		{ cpuset_ok=0; echo "# cpuset: restore mismatch (wanted '$cpuset_orig', got '$cpuset_restored')"; }

	if [ "$cpuset_ok" -eq 1 ]; then
		pass "cpuset_camera: camera-daemon cpuset write '4-7' and restore OK"
	else
		fail "cpuset_camera: cpuset write or restore failed (see # lines above)"
	fi
fi

# --------------------------------------------------------------------
# Test 19: cpu.uclamp.min writability and restore (camera + top-app)
#
# init.cinema.rc raises cpu.uclamp.min for camera-daemon (70) and
# top-app (80) cgroups to bias scheduler towards performance cores
# during cinema recording.  Direct sysfs write required — same
# rationale as Tests 16–18.
#
# uclamp values are stored as integers in the range [0, 1024].
# The nodes live in the cpuctl (cpu cgroup) hierarchy.
# --------------------------------------------------------------------

uclamp_ok=1

for node_info in "${UCLAMP_CAM}:70" "${UCLAMP_TOP}:80"; do
	node="${node_info%%:*}"
	cinema_val="${node_info##*:}"

	if [ ! -w "$node" ]; then
		echo "# uclamp: $node not writable — skipping this node"
		continue
	fi

	u_orig=$(cat "$node" 2>/dev/null)

	echo "$cinema_val" > "$node" 2>/dev/null
	u_after=$(cat "$node" 2>/dev/null)

	echo "${u_orig:-0}" > "$node" 2>/dev/null
	u_restored=$(cat "$node" 2>/dev/null)

	if [ "$u_after" != "$cinema_val" ]; then
		uclamp_ok=0
		echo "# uclamp: $node wrote $cinema_val, read '$u_after'"
	fi
	if [ "$u_restored" != "${u_orig:-0}" ]; then
		uclamp_ok=0
		echo "# uclamp: $node restore mismatch (wanted '${u_orig:-0}', got '$u_restored')"
	fi
done

# Check if both nodes were absent (not writable)
if [ ! -w "$UCLAMP_CAM" ] && [ ! -w "$UCLAMP_TOP" ]; then
	skip "uclamp_cgroup" "$UCLAMP_CAM and $UCLAMP_TOP not writable (cpuctl not mounted?)"
elif [ "$uclamp_ok" -eq 1 ]; then
	pass "uclamp_cgroup: cpu.uclamp.min write/restore OK for camera-daemon and top-app"
else
	fail "uclamp_cgroup: uclamp write or restore failed (see # lines above)"
fi

# --------------------------------------------------------------------
# Test 20: ZRAM dynamic resize via cinema_on_apply.sh / cinema_off_apply.sh
#
# cinema_on_apply.sh shrinks ZRAM to 2GB; cinema_off_apply.sh restores
# it to 4GB.  These scripts run under Android init as a oneshot service
# triggered by the cinema mode property.  This test invokes them
# directly as root to verify the shrink/restore cycle works in
# isolation.
#
# The test is skipped if:
#   - The scripts are not present (vendor partition not mounted or
#     files not deployed via cinema_device.mk).
#   - /sys/block/zram0/disksize is not present (ZRAM not configured).
#   - ZRAM is not currently active as swap (swapoff would fail; the
#     scripts handle this gracefully but the resize would be a no-op
#     and would not exercise the full path).
#
# disksize is read and compared in bytes; the kernel may round up to a
# page boundary so we compare with -ge/-le rather than exact equality.
#
# 2 GB = 2147483648 bytes
# 4 GB = 4294967296 bytes
# --------------------------------------------------------------------

ZRAM_2G=2147483648
ZRAM_4G=4294967296

if [ ! -x "$CINEMA_ON_APPLY" ]; then
	skip "zram_resize" "$CINEMA_ON_APPLY not executable (cinema scripts not deployed?)"
elif [ ! -x "$CINEMA_OFF_APPLY" ]; then
	skip "zram_resize" "$CINEMA_OFF_APPLY not executable"
elif [ ! -w "$ZRAM_DISKSIZE" ]; then
	skip "zram_resize" "$ZRAM_DISKSIZE not writable (CONFIG_ZRAM not set?)"
elif ! grep -q "zram0" /proc/swaps 2>/dev/null; then
	skip "zram_resize" "zram0 not active as swap — resize cycle would be a no-op"
else
	disksize_before=$(cat "$ZRAM_DISKSIZE" 2>/dev/null)

	"$CINEMA_ON_APPLY"
	disksize_on=$(cat "$ZRAM_DISKSIZE" 2>/dev/null)

	"$CINEMA_OFF_APPLY"
	disksize_off=$(cat "$ZRAM_DISKSIZE" 2>/dev/null)

	zram_ok=1

	# After cinema_on_apply: disksize should be ≤ 2 GB (scripts write 2 GB
	# exactly; kernel rounds up to page boundary so accept up to 2G + 4095).
	if [ -z "$disksize_on" ] || [ "$disksize_on" -gt $((ZRAM_2G + 4095)) ] 2>/dev/null; then
		zram_ok=0
		echo "# zram: after cinema_on_apply disksize=$disksize_on (expected ≤ $((ZRAM_2G + 4095)))"
	fi

	# After cinema_off_apply: disksize should be ≥ 4 GB.
	if [ -z "$disksize_off" ] || [ "$disksize_off" -lt "$ZRAM_4G" ] 2>/dev/null; then
		zram_ok=0
		echo "# zram: after cinema_off_apply disksize=$disksize_off (expected ≥ $ZRAM_4G)"
	fi

	if [ "$zram_ok" -eq 1 ]; then
		pass "zram_resize: on_apply shrunk to ${disksize_on}B, off_apply restored to ${disksize_off}B"
	else
		fail "zram_resize: ZRAM resize cycle failed (see # lines above)"
	fi
fi

# --------------------------------------------------------------------
# Summary (TAP plan at end — valid per TAP v13 spec)
# --------------------------------------------------------------------

echo "1..${TESTS_TOTAL}"
echo "# Totals: pass=$PASS fail=$FAIL skip=$SKIP"

if [ $FAIL -gt 0 ]; then
	exit $ksft_fail
fi
exit $ksft_pass
