#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# sm8650_topology.sh — SM8650 (Pineapple) cpufreq topology validator
#
# Requires: bash >= 4.0 (associative arrays), root, SM8650 target.
#
# This script reads the runtime cpufreq policy topology from sysfs and
# validates it against the known SM8650 specification:
#
#   Domain 0 — Efficiency   (Cortex-A520 ×2):   CPUs 0-1, governing CPU 0
#   Domain 1 — Perf-Lo      (Cortex-A720 ×2):   CPUs 2-3, governing CPU 2
#   Domain 2 — Perf-Hi      (Cortex-A720 ×3):   CPUs 4-6, governing CPU 4
#   Domain 3 — Prime        (Cortex-X4 ×1):     CPU 7,    governing CPU 7
#
# Validation steps:
#   1. Confirm exactly 4 cpufreq policies exist.
#   2. Confirm cluster sizes are exactly [2, 2, 3, 1].
#   3. Confirm governing CPU for each domain (policy->cpu).
#   4. Confirm strict frequency ordering: A520 < A720-lo < A720-hi < X4.
#   5. Plausibility: X4 prime core max_freq >= 3.3 GHz.
#   6. Domain membership: every CPU 0-7 in exactly one domain.
#   7. Cinema-mode integration: enable driver, verify each policy's
#      scaling_min_freq == cpuinfo_max_freq (driver pinned correctly).
#   8. Verify unpin: after disable, at least one policy is released.
#
# Exit codes:
#   0 = PASS  (all applicable checks passed)
#   1 = FAIL  (at least one check failed)
#   4 = SKIP  (not running on SM8650 hardware or dependencies absent)
#
# TAP note: plan header (1..N) is emitted at the end since total is
# not known until all tests complete.  This is valid per TAP v13 spec.

# Require bash >= 4.0 for associative arrays
if [ -z "${BASH_VERSION}" ] || \
   [ "${BASH_VERSINFO[0]}" -lt 4 ]; then
	echo "TAP version 13"
	echo "1..0 # SKIP requires bash >= 4.0"
	exit 4
fi

ksft_pass=0
ksft_fail=1
ksft_skip=4

PASS=0
FAIL=0
SKIP=0
TESTS_TOTAL=0

CPUFREQ_ROOT="/sys/devices/system/cpu/cpufreq"
CPU_ROOT="/sys/devices/system/cpu"
SYSFS_ENABLE="/sys/kernel/cinema_mode/enable"
MODULE_NAME="cinema_mode"

# SM8650 expected topology constants
SM8650_NCPUS=8
SM8650_NDOMAINS=4
SM8650_CLUSTER_SIZES=(2 2 3 1)        # indexed by ascending policy number
SM8650_GOVERNING_CPUS=(0 2 4 7)
# Minimum plausible max-frequency for each cluster in kHz
SM8650_MIN_FREQS=(2200000 2900000 3100000 3300000)

# --------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------

pass() { echo "ok $((++TESTS_TOTAL)) $1"; PASS=$((PASS+1)); }
fail() { echo "not ok $((++TESTS_TOTAL)) $1"; [ -n "$2" ] && echo "# $2"; FAIL=$((FAIL+1)); }
skip() { echo "ok $((++TESTS_TOTAL)) $1 # SKIP $2"; SKIP=$((SKIP+1)); }

die_skip() { echo "1..0 # SKIP $1"; exit $ksft_skip; }

# Cleanup: unload module and disable cinema mode on any exit path.
cleanup() {
	echo 0 > "$SYSFS_ENABLE" 2>/dev/null || true
	rmmod "$MODULE_NAME" 2>/dev/null || true
}
trap cleanup EXIT

read_file() { cat "$1" 2>/dev/null | tr -d '\n\r'; }

policy_cpu_list() {
	local policy_dir="$1"
	read_file "${policy_dir}/related_cpus" | tr ' ' '\n' | sort -n
}

policy_cpu_count() {
	local policy_dir="$1"
	policy_cpu_list "$policy_dir" | grep -c '[0-9]'
}

policy_governing_cpu() {
	local policy_dir="$1"
	read_file "${policy_dir}/affected_cpus" | awk '{print $1}'
}

# --------------------------------------------------------------------
# Prerequisites
# --------------------------------------------------------------------

echo "TAP version 13"

if [ "$(id -u)" -ne 0 ]; then
	die_skip "must be run as root"
fi

if [ ! -d "$CPUFREQ_ROOT" ]; then
	die_skip "cpufreq sysfs not available"
fi

# Detect CPU count
ncpus=$(ls -d "${CPU_ROOT}"/cpu[0-9]* 2>/dev/null | wc -l)
if [ "$ncpus" -ne "$SM8650_NCPUS" ]; then
	die_skip "found $ncpus CPUs, SM8650 topology requires exactly $SM8650_NCPUS"
fi

# Detect cpufreq driver
driver=$(read_file "${CPUFREQ_ROOT}/policy0/scaling_driver" 2>/dev/null)
if [[ "$driver" != *"qcom"* ]] && [[ "$driver" != *"cpufreq-hw"* ]] && \
   [[ "$driver" != *"cpufreq-epss"* ]]; then
	die_skip "cpufreq driver '$driver' is not a Qualcomm EPSS driver"
fi

# --------------------------------------------------------------------
# Collect and sort policy directories by numeric policy index.
# --------------------------------------------------------------------

policy_dirs=()
for pd in "${CPUFREQ_ROOT}"/policy*; do
	[ -d "$pd" ] && policy_dirs+=("$pd")
done
IFS=$'\n' policy_dirs=($(printf '%s\n' "${policy_dirs[@]}" | sort -V)); unset IFS

ndomains=${#policy_dirs[@]}

# --------------------------------------------------------------------
# Test 1: Exactly 4 cpufreq policy directories
# --------------------------------------------------------------------

if [ "$ndomains" -eq "$SM8650_NDOMAINS" ]; then
	pass "domain_count: found $ndomains frequency domains (expected $SM8650_NDOMAINS)"
else
	fail "domain_count: found $ndomains, expected $SM8650_NDOMAINS"
	echo "1..1"
	exit $ksft_fail
fi

# --------------------------------------------------------------------
# Test 2: Cluster sizes are [2, 2, 3, 1]
# --------------------------------------------------------------------

sizes_ok=1
for i in "${!policy_dirs[@]}"; do
	actual=$(policy_cpu_count "${policy_dirs[$i]}")
	expected="${SM8650_CLUSTER_SIZES[$i]}"
	if [ "$actual" -ne "$expected" ]; then
		sizes_ok=0
		echo "# domain $i ($(basename "${policy_dirs[$i]}")): $actual CPUs, expected $expected"
	fi
done

if [ "$sizes_ok" -eq 1 ]; then
	pass "cluster_sizes: [2, 2, 3, 1] layout confirmed (A520/A720-lo/A720-hi/X4)"
else
	fail "cluster_sizes: unexpected cluster layout (see # lines above)"
fi

# --------------------------------------------------------------------
# Test 3: Governing CPU indices are [0, 2, 4, 7]
# --------------------------------------------------------------------

gov_ok=1
for i in "${!policy_dirs[@]}"; do
	actual=$(policy_governing_cpu "${policy_dirs[$i]}")
	expected="${SM8650_GOVERNING_CPUS[$i]}"
	if [ "$actual" != "$expected" ]; then
		gov_ok=0
		echo "# domain $i: governing CPU is $actual, expected $expected"
	fi
done

if [ "$gov_ok" -eq 1 ]; then
	pass "governing_cpus: CPUs 0, 2, 4, 7 are domain governors (matches policy->cpu)"
else
	fail "governing_cpus: unexpected governing CPU assignment"
fi

# --------------------------------------------------------------------
# Test 4: Frequency ordering — strict ascending A520 < A720-lo < A720-hi < X4
# --------------------------------------------------------------------

max_freqs=()
for pd in "${policy_dirs[@]}"; do
	f=$(read_file "${pd}/cpuinfo_max_freq")
	max_freqs+=("${f:-0}")
done

freq_ok=1
for ((i=1; i<SM8650_NDOMAINS; i++)); do
	if [ "${max_freqs[$i]}" -le "${max_freqs[$((i-1))]}" ]; then
		freq_ok=0
		echo "# domain $i freq ${max_freqs[$i]}kHz not > domain $((i-1)) freq ${max_freqs[$((i-1))]}kHz"
	fi
done

if [ "$freq_ok" -eq 1 ]; then
	pass "freq_ordering: A520(${max_freqs[0]}kHz) < A720-lo(${max_freqs[1]}kHz) < A720-hi(${max_freqs[2]}kHz) < X4(${max_freqs[3]}kHz)"
else
	fail "freq_ordering: expected strict ascending, got ${max_freqs[*]}kHz"
fi

# --------------------------------------------------------------------
# Test 5: Plausibility — X4 prime core must be >= 3.3 GHz on SM8650
# --------------------------------------------------------------------

prime_max="${max_freqs[3]}"
if [ "${prime_max:-0}" -ge 3300000 ]; then
	pass "prime_plausible: X4 prime max freq ${prime_max}kHz >= 3300000kHz (3.3 GHz)"
else
	fail "prime_plausible: X4 prime max freq ${prime_max}kHz is below SM8650 spec (>= 3.3 GHz)"
fi

# --------------------------------------------------------------------
# Test 6: Domain CPU membership — every CPU 0-7 belongs to exactly one
#          domain; no CPU is orphaned or double-assigned.
# --------------------------------------------------------------------

declare -A cpu_domain_count
for cpu in $(seq 0 $((SM8650_NCPUS - 1))); do
	cpu_domain_count[$cpu]=0
done

for i in "${!policy_dirs[@]}"; do
	while IFS= read -r cpu; do
		[ -n "$cpu" ] || continue
		cpu_domain_count[$cpu]=$((cpu_domain_count[$cpu] + 1))
	done < <(policy_cpu_list "${policy_dirs[$i]}")
done

membership_ok=1
for cpu in $(seq 0 $((SM8650_NCPUS - 1))); do
	count="${cpu_domain_count[$cpu]}"
	if [ "$count" -ne 1 ]; then
		membership_ok=0
		echo "# CPU$cpu belongs to $count domains (expected 1)"
	fi
done

if [ "$membership_ok" -eq 1 ]; then
	pass "cpu_membership: all 8 CPUs belong to exactly one frequency domain"
else
	fail "cpu_membership: CPU domain assignment is not a partition"
fi

# --------------------------------------------------------------------
# Tests 7 & 8: Cinema-mode integration — pin and unpin verification
# --------------------------------------------------------------------

cinema_available=false
if grep -qw "$MODULE_NAME" /proc/modules 2>/dev/null || \
   modprobe "$MODULE_NAME" 2>/dev/null || \
   insmod "${MODULE_NAME}.ko" 2>/dev/null; then
	if [ -f "$SYSFS_ENABLE" ]; then
		cinema_available=true
	fi
fi

if ! $cinema_available; then
	skip "cinema_pin"   "cinema_mode module not available"
	skip "cinema_unpin" "cinema_mode module not available"
else
	# ---- Test 7: pin ----

	gov_name=$(read_file "${policy_dirs[0]}/scaling_governor")
	if [ "$gov_name" = "performance" ]; then
		skip "cinema_pin" "performance governor already enforces min=max"
	else
		echo 1 > "$SYSFS_ENABLE" 2>/dev/null
		sleep 0.2

		pin_ok=1
		for i in "${!policy_dirs[@]}"; do
			pd="${policy_dirs[$i]}"
			min=$(read_file "${pd}/scaling_min_freq")
			max=$(read_file "${pd}/cpuinfo_max_freq")
			if [ -z "$min" ] || [ -z "$max" ]; then
				echo "# domain $i: could not read freq files"
				pin_ok=0
				continue
			fi
			if [ "$min" -ne "$max" ]; then
				pin_ok=0
				echo "# domain $i ($(basename "$pd")): min=${min}kHz max=${max}kHz — NOT pinned"
			else
				echo "# domain $i ($(basename "$pd")): min=${min}kHz == max — PINNED OK"
			fi
		done

		if [ "$pin_ok" -eq 1 ]; then
			pass "cinema_pin: all $SM8650_NDOMAINS domains pinned to cpuinfo_max_freq"
		else
			fail "cinema_pin: one or more domains not pinned"
		fi
	fi

	# ---- Test 8: unpin ----

	echo 0 > "$SYSFS_ENABLE" 2>/dev/null
	sleep 0.2

	pinned_after=0
	for pd in "${policy_dirs[@]}"; do
		min=$(read_file "${pd}/scaling_min_freq")
		max=$(read_file "${pd}/cpuinfo_max_freq")
		[ -z "$min" ] || [ -z "$max" ] && continue
		[ "$min" -eq "$max" ] && pinned_after=$((pinned_after + 1))
	done

	if [ "$pinned_after" -lt "$SM8650_NDOMAINS" ]; then
		pass "cinema_unpin: at least one domain released QoS floor after disable"
	else
		gov_check=$(read_file "${policy_dirs[0]}/scaling_governor")
		if [ "$gov_check" = "performance" ]; then
			skip "cinema_unpin" "performance governor keeps min=max regardless"
		else
			fail "cinema_unpin: all domains still pinned after disable (governor: $gov_check)"
		fi
	fi

	rmmod "$MODULE_NAME" 2>/dev/null || true
fi

# --------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------

echo "1..${TESTS_TOTAL}"
echo "# Totals: pass=$PASS fail=$FAIL skip=$SKIP"
echo "# SM8650 topology: ${ndomains} domains, sizes=${SM8650_CLUSTER_SIZES[*]}, governors=${SM8650_GOVERNING_CPUS[*]}"
echo "# cpufreq driver: $driver"

[ $FAIL -gt 0 ] && exit $ksft_fail
exit $ksft_pass
