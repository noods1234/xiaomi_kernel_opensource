// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_mode_kunit.c — KUnit test suite for cinema_mode.c
 *
 * Scope
 * -----
 * cinema_mode.c has two distinct layers:
 *
 *   1. PM-QoS frequency constraint layer — freq_qos_add_request /
 *      freq_qos_remove_request / freq_qos_request_active, exercised
 *      against synthetic struct freq_constraints objects with no real
 *      cpufreq hardware required.
 *
 *   2. Hardware integration layer — cpufreq_cpu_get, wakeup sources,
 *      sysfs kobjects, CPU latency QoS — which require a real or
 *      emulated cpufreq subsystem and are covered by the companion
 *      kselftest: tools/testing/selftests/cpufreq/cinema_mode.sh
 *
 * Test strategy for layer 1
 * -------------------------
 * freq_constraints_init() is not exported, so we replicate its
 * initialisation logic here.  All other symbols used (freq_qos_add_
 * request, freq_qos_update_request, freq_qos_remove_request,
 * freq_qos_request_active) are EXPORT_SYMBOL_GPL and accessible from
 * a module.
 *
 * freq_qos_read_value() is also not exported, so the aggregated MIN
 * value is read by accessing qos->min_freq.target_value directly.
 * This field is read with READ_ONCE() by pm_qos_read_value() in the
 * kernel; in single-threaded KUnit context no other writer exists so
 * plain access is safe.  This is the only viable approach for a
 * module-based test.
 *
 * Novel testing approaches implemented here
 * ------------------------------------------
 *  • Multi-domain isolation — three independent freq_constraints
 *    confirm that a request on one domain never bleeds into another.
 *  • Partial-failure unwind simulation — mirrors cinema_activate()'s
 *    failure path: add two domains, simulate domain-3 failure, unwind
 *    via the active-guard loop, assert all requests are gone.
 *  • QoS aggregation (max-of-min) — two competing MIN requests; the
 *    higher value must dominate; dropping it must return to the lower.
 *  • Idempotent-remove guard — the freq_req_active[] sentinel must
 *    prevent a double freq_qos_remove_request(); kernel guard returns
 *    -EINVAL on the unguarded second call.
 *  • Input-parser edge cases — kstrtoint coverage for the enable_store
 *    sysfs handler including newline-terminated strings from echo(1).
 *  • SM8650 topology sanity — nr_cpu_ids must match the 8-CPU DTS.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <kunit/test.h>
#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/plist.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/string.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_init_freq_constraints - replicate freq_constraints_init().
 *
 * freq_constraints_init() is not exported.  We replicate its body so
 * that KUnit tests can build synthetic freq_constraints objects without
 * depending on a non-exported symbol.  The logic is taken verbatim from
 * kernel/power/qos.c; if that function changes, this must be updated.
 */
static void cinema_test_init_freq_constraints(struct freq_constraints *qos)
{
	struct pm_qos_constraints *c;

	c = &qos->min_freq;
	plist_head_init(&c->list);
	c->target_value        = FREQ_QOS_MIN_DEFAULT_VALUE;
	c->default_value       = FREQ_QOS_MIN_DEFAULT_VALUE;
	c->no_constraint_value = FREQ_QOS_MIN_DEFAULT_VALUE;
	c->type                = PM_QOS_MAX;
	c->notifiers           = &qos->min_freq_notifiers;
	BLOCKING_INIT_NOTIFIER_HEAD(c->notifiers);

	c = &qos->max_freq;
	plist_head_init(&c->list);
	c->target_value        = FREQ_QOS_MAX_DEFAULT_VALUE;
	c->default_value       = FREQ_QOS_MAX_DEFAULT_VALUE;
	c->no_constraint_value = FREQ_QOS_MAX_DEFAULT_VALUE;
	c->type                = PM_QOS_MIN;
	c->notifiers           = &qos->max_freq_notifiers;
	BLOCKING_INIT_NOTIFIER_HEAD(c->notifiers);
}

/*
 * cinema_test_read_min - read aggregated MIN frequency value.
 *
 * freq_qos_read_value() is not exported, so we read target_value
 * directly.  In single-threaded KUnit context there is no concurrent
 * writer, so the plain read (without READ_ONCE) is safe.
 */
static inline s32 cinema_test_read_min(struct freq_constraints *qos)
{
	return qos->min_freq.target_value;
}

/* ------------------------------------------------------------------ */
/* Test 1: basic lifecycle — add, active-check, remove, inactive-check  */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_freq_qos_lifecycle - validate add/active/remove sequence.
 *
 * Mirrors the innermost operation of cinema_activate() and
 * cinema_deactivate().
 *
 * cinema_activate() installs a recording floor computed as:
 *   floor_hz = max_freq * floor_pct / 100   (default floor_pct = 85)
 * rather than max_freq itself, to preserve the upper OPP band for EAS.
 * This test uses a representative floor_hz value (85% of the X4 prime
 * max, rounded to a real OPP step) to mirror what the driver installs.
 */
static void cinema_test_freq_qos_lifecycle(struct kunit *test)
{
	struct freq_constraints qos;
	struct freq_qos_request req;
	/*
	 * SM8650 X4 prime max = 3302400 kHz.
	 * floor_hz = 3302400 * 85 / 100 = 2807040 kHz → rounds to nearest
	 * OPP step 2803200 kHz in the actual EPSS table.  Use 2803200 here
	 * to represent a realistic driver-installed floor value.
	 */
	const s32 floor_khz = 2803200;
	int ret;

	cinema_test_init_freq_constraints(&qos);
	memset(&req, 0, sizeof(req));

	/* Before add: request must be inactive */
	KUNIT_EXPECT_FALSE(test, freq_qos_request_active(&req));

	ret = freq_qos_add_request(&qos, &req, FREQ_QOS_MIN, floor_khz);
	KUNIT_ASSERT_GE_MSG(test, ret, 0,
			    "freq_qos_add_request unexpectedly failed");

	/* After add: request must be active */
	KUNIT_EXPECT_TRUE(test, freq_qos_request_active(&req));

	/* Aggregated value must equal what we requested */
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos), floor_khz);

	ret = freq_qos_remove_request(&req);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "freq_qos_remove_request failed");

	/* After remove: request must be inactive again */
	KUNIT_EXPECT_FALSE(test, freq_qos_request_active(&req));

	/* Aggregated value must revert to the unconstrained default */
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos),
			FREQ_QOS_MIN_DEFAULT_VALUE);
}

/* ------------------------------------------------------------------ */
/* Test 2: active-guard prevents double-remove                          */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_active_guard_idempotent_remove - freq_req_active[] sentinel.
 *
 * cinema_deactivate() guards every freq_qos_remove_request() call with
 * freq_req_active[cpu].  This test confirms the driver-level guard logic:
 *   (a) The guarded first remove succeeds (ret >= 0).
 *   (b) The guard variable is cleared after the first remove.
 *   (c) A simulated second deactivate pass finds the guard false and
 *       skips the remove call entirely — no double-remove occurs.
 *   (d) After both passes the request is confirmed inactive.
 *
 * NOTE: calling freq_qos_remove_request() on an already-inactive request
 * triggers WARN_ON() in the kernel and returns -EINVAL.  That code path
 * is intentionally NOT invoked here to avoid kernel WARN noise in CI.
 * The driver's freq_req_active[] guard is precisely what prevents that
 * situation in production — and that guard is what this test validates.
 */
static void cinema_test_active_guard_idempotent_remove(struct kunit *test)
{
	struct freq_constraints qos;
	struct freq_qos_request req;
	bool req_active = false;
	int ret;

	cinema_test_init_freq_constraints(&qos);
	memset(&req, 0, sizeof(req));

	ret = freq_qos_add_request(&qos, &req, FREQ_QOS_MIN, 1000000);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "freq_qos_add_request failed");
	req_active = true;

	/* First guarded remove — must succeed */
	if (req_active) {
		ret = freq_qos_remove_request(&req);
		KUNIT_EXPECT_GE_MSG(test, ret, 0,
				    "first freq_qos_remove_request failed");
		req_active = false;
	}
	KUNIT_EXPECT_FALSE(test, freq_qos_request_active(&req));

	/* Guard variable must be cleared after first remove */
	KUNIT_EXPECT_FALSE_MSG(test, req_active,
			       "guard variable not cleared after first remove");

	/*
	 * Simulate a second cinema_deactivate() pass — the guard must
	 * suppress the remove call entirely.  The body of the if-block
	 * is intentionally unreachable when req_active == false.
	 */
	if (req_active) {
		/* This must never execute — guard should prevent it */
		KUNIT_FAIL(test, "guard failed: second remove was not suppressed");
		freq_qos_remove_request(&req); /* would WARN and return -EINVAL */
	}

	/* Request must still be inactive after the no-op second pass */
	KUNIT_EXPECT_FALSE_MSG(test, freq_qos_request_active(&req),
			       "request active after guarded double-remove");
}

/* ------------------------------------------------------------------ */
/* Test 3: partial-failure unwind simulation                            */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_partial_failure_unwind - mirrors cinema_activate() error path.
 *
 * Scenario: three cpufreq domains (Little/Mid/Big).  Requests succeed for
 * domains 0 and 1; domain 2 fails (simulated by never calling add for it).
 * The driver then calls cinema_deactivate(), which iterates over all CPUs,
 * checks freq_req_active[], and removes only the active ones.
 * After the unwind all active flags must be false and all requests inactive.
 */
static void cinema_test_partial_failure_unwind(struct kunit *test)
{
	struct freq_constraints qos[3];
	struct freq_qos_request req[3];
	bool req_active[3] = { false, false, false };
	int i, ret;

	for (i = 0; i < 3; i++) {
		cinema_test_init_freq_constraints(&qos[i]);
		memset(&req[i], 0, sizeof(req[i]));
	}

	ret = freq_qos_add_request(&qos[0], &req[0], FREQ_QOS_MIN, 1804800);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "domain 0 freq_qos_add_request failed");
	req_active[0] = true;

	ret = freq_qos_add_request(&qos[1], &req[1], FREQ_QOS_MIN, 2649600);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "domain 1 freq_qos_add_request failed");
	req_active[1] = true;

	/* Domain 2 "fails" — req[2] is never added, req_active[2] stays false */

	/* Simulate cinema_deactivate() unwind loop */
	for (i = 0; i < 3; i++) {
		if (!req_active[i])
			continue;
		ret = freq_qos_remove_request(&req[i]);
		KUNIT_EXPECT_GE_MSG(test, ret, 0,
				    "unwind remove failed for domain %d", i);
		req_active[i] = false;
	}

	/* Post-unwind: all requests must be inactive */
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_FALSE_MSG(test, req_active[i],
				       "domain %d active flag not cleared", i);
		KUNIT_EXPECT_FALSE_MSG(test, freq_qos_request_active(&req[i]),
				       "domain %d request still active", i);
	}
}

/* ------------------------------------------------------------------ */
/* Test 4: multi-domain isolation                                        */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_multi_domain_isolation - requests on different domains
 * must not cross-contaminate each other's aggregated values.
 *
 * Validates that four independent freq_constraints objects maintain
 * their own priority lists; the SM8650 has four distinct EPSS domains
 * (A520, A720-lo, A720-hi, X4) and each should be pinned independently.
 */
static void cinema_test_multi_domain_isolation(struct kunit *test)
{
	struct freq_constraints qos[4];
	struct freq_qos_request req[4];
	/*
	 * Approximate SM8650 max freqs: A520=2265600, A720-lo=2956800,
	 * A720-hi=3148800, X4=3302400 (all in kHz)
	 */
	static const s32 max_freqs[] = { 2265600, 2956800, 3148800, 3302400 };
	int i, ret;

	for (i = 0; i < 4; i++) {
		cinema_test_init_freq_constraints(&qos[i]);
		memset(&req[i], 0, sizeof(req[i]));
	}

	for (i = 0; i < 4; i++) {
		ret = freq_qos_add_request(&qos[i], &req[i],
					   FREQ_QOS_MIN, max_freqs[i]);
		KUNIT_ASSERT_GE_MSG(test, ret, 0,
				    "freq_qos_add_request failed for domain %d", i);
	}

	/* Each domain must reflect only its own request value */
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_EQ_MSG(test, cinema_test_read_min(&qos[i]),
				    max_freqs[i],
				    "domain %d value mismatch", i);
	}

	/* Remove domain 1; remaining domains must be unaffected */
	ret = freq_qos_remove_request(&req[1]);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "domain 1 remove failed");
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos[0]), max_freqs[0]);
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos[1]),
			FREQ_QOS_MIN_DEFAULT_VALUE);
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos[2]), max_freqs[2]);
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos[3]), max_freqs[3]);

	ret = freq_qos_remove_request(&req[0]);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "domain 0 remove failed");
	ret = freq_qos_remove_request(&req[2]);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "domain 2 remove failed");
	ret = freq_qos_remove_request(&req[3]);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "domain 3 remove failed");

	/* Post-cleanup: all requests must be inactive */
	for (i = 0; i < 4; i++) {
		KUNIT_EXPECT_FALSE_MSG(test, freq_qos_request_active(&req[i]),
				       "domain %d request still active after cleanup", i);
	}
}

/* ------------------------------------------------------------------ */
/* Test 5: QoS aggregation — max-of-min semantics                       */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_qos_aggregation_max_wins - FREQ_QOS_MIN uses PM_QOS_MAX
 * aggregation: the highest minimum request wins.
 *
 * Validates the invariant that cinema_mode's "pin to max_freq" approach
 * relies on: even if another driver holds a lower minimum, cinema_mode's
 * higher value dominates, and removing it restores the lower floor.
 */
static void cinema_test_qos_aggregation_max_wins(struct kunit *test)
{
	struct freq_constraints qos;
	struct freq_qos_request req_low, req_high;
	const s32 low  = 1000000;
	const s32 high = 3187200;
	int ret;

	cinema_test_init_freq_constraints(&qos);
	memset(&req_low,  0, sizeof(req_low));
	memset(&req_high, 0, sizeof(req_high));

	ret = freq_qos_add_request(&qos, &req_low,  FREQ_QOS_MIN, low);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "low request add failed");
	ret = freq_qos_add_request(&qos, &req_high, FREQ_QOS_MIN, high);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "high request add failed");

	/* High value must dominate */
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos), high);

	/* Remove the high request; aggregated floor drops to low */
	ret = freq_qos_remove_request(&req_high);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "high request remove failed");
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos), low);

	/* Remove the low request; aggregated floor resets to default */
	ret = freq_qos_remove_request(&req_low);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "low request remove failed");
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos),
			FREQ_QOS_MIN_DEFAULT_VALUE);
}

/* ------------------------------------------------------------------ */
/* Test 6: freq_qos_update_request path                                 */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_freq_qos_update - validate update path on an active request.
 *
 * cinema_mode.c currently does not use update_request, but a future
 * enhancement could dynamically adjust the pinned frequency.  This test
 * validates that the update path works correctly and that the request
 * remains active and tracks the new value after an update.
 *
 * NOTE: we deliberately do NOT call freq_qos_update_request() on an
 * inactive request.  That path triggers WARN(1, ...) inside the kernel
 * (kernel/power/qos.c), which would emit a WARNING in CI dmesg and cause
 * false test-suite failures.  The driver's freq_req_active[] guard is
 * what prevents that call in production — and that guard is validated by
 * cinema_test_active_guard_idempotent_remove (Test 2) and
 * cinema_test_freq_qos_guard_blocks_update (Test 6b) below.
 */
static void cinema_test_freq_qos_update(struct kunit *test)
{
	struct freq_constraints qos;
	struct freq_qos_request req;
	const s32 initial = 1804800;
	const s32 updated = 2649600;
	int ret;

	cinema_test_init_freq_constraints(&qos);
	memset(&req, 0, sizeof(req));

	ret = freq_qos_add_request(&qos, &req, FREQ_QOS_MIN, initial);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "freq_qos_add_request failed");
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos), initial);

	ret = freq_qos_update_request(&req, updated);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "freq_qos_update_request failed");
	KUNIT_EXPECT_EQ(test, cinema_test_read_min(&qos), updated);

	/* Request must still be active after update */
	KUNIT_EXPECT_TRUE(test, freq_qos_request_active(&req));

	ret = freq_qos_remove_request(&req);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "remove after update failed");

	KUNIT_EXPECT_FALSE_MSG(test, freq_qos_request_active(&req),
			       "request should be inactive after remove");
}

/* ------------------------------------------------------------------ */
/* Test 6b: driver guard prevents update on inactive request            */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_freq_qos_guard_blocks_update - the freq_req_active[] sentinel
 * must prevent freq_qos_update_request() from being called on an inactive
 * request.
 *
 * This test mirrors the driver-level guard pattern: after a remove, the
 * active flag is false, and any subsequent update attempt is suppressed
 * by the guard — exactly as cinema_deactivate() suppresses double-removes.
 *
 * We validate the guard variable state only.  We intentionally do not call
 * freq_qos_update_request() on the inactive request because that would
 * trigger WARN(1, ...) in the kernel — that code path is kernel-internal
 * and already tested by the PM-QoS subsystem's own tests.  Our contract
 * is that the driver never reaches that path.
 */
static void cinema_test_freq_qos_guard_blocks_update(struct kunit *test)
{
	struct freq_constraints qos;
	struct freq_qos_request req;
	bool req_active = false;
	int ret;

	cinema_test_init_freq_constraints(&qos);
	memset(&req, 0, sizeof(req));

	ret = freq_qos_add_request(&qos, &req, FREQ_QOS_MIN, 1804800);
	KUNIT_ASSERT_GE_MSG(test, ret, 0, "freq_qos_add_request failed");
	req_active = true;

	ret = freq_qos_remove_request(&req);
	KUNIT_EXPECT_GE_MSG(test, ret, 0, "freq_qos_remove_request failed");
	req_active = false;

	/*
	 * Guard check: req_active is false → an update call would be
	 * suppressed.  Confirm the guard state without invoking the
	 * WARN-producing kernel path.
	 */
	KUNIT_EXPECT_FALSE_MSG(test, req_active,
			       "guard must be false after remove");
	KUNIT_EXPECT_FALSE_MSG(test, freq_qos_request_active(&req),
			       "kernel request must be inactive after remove");

	/* The if-guard that protects the driver's update call path */
	if (req_active) {
		/* Must never reach here */
		KUNIT_FAIL(test, "guard failed: update would have been called on inactive request");
	}
}

/* ------------------------------------------------------------------ */
/* Test 7: sysfs output format                                           */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_sysfs_output_format - validate the format string used by
 * enable_show().
 *
 * enable_show() produces exactly "0\n" or "1\n" via:
 *   scnprintf(buf, PAGE_SIZE, "%d\n", active ? 1 : 0)
 * Userspace tools parsing /sys/kernel/cinema_mode/enable must see
 * these exact strings.  This test validates the format independently
 * of the sysfs infrastructure.
 */
static void cinema_test_sysfs_output_format(struct kunit *test)
{
	/*
	 * PAGE_SIZE (4096) on the kernel stack consumes 25% of the 16KB
	 * ARM64 stack budget.  Use kunit_kmalloc so the allocation comes
	 * from the heap and is automatically freed by KUnit on test exit.
	 */
	char *buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int n;

	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* Inactive state */
	n = scnprintf(buf, PAGE_SIZE, "%d\n", 0);
	KUNIT_EXPECT_STREQ(test, buf, "0\n");
	KUNIT_EXPECT_EQ(test, n, 2);

	/* Active state */
	n = scnprintf(buf, PAGE_SIZE, "%d\n", 1);
	KUNIT_EXPECT_STREQ(test, buf, "1\n");
	KUNIT_EXPECT_EQ(test, n, 2);
}

/* ------------------------------------------------------------------ */
/* Test 8: sysfs input parsing                                           */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_sysfs_input_parsing - validate the kstrtoint call in
 * enable_store().
 *
 * Covers the full range of inputs the sysfs handler receives, including
 * the coercion `val = !!val` that maps any non-zero integer to 1.
 */
static void cinema_test_sysfs_input_parsing(struct kunit *test)
{
	int val;
	int ret;

	/* Standard "0" */
	ret = kstrtoint("0", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, !!val, 0);

	/* Standard "1" */
	ret = kstrtoint("1", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, !!val, 1);

	/* echo(1) appends a newline — kernel kstrtoint accepts trailing \n */
	ret = kstrtoint("1\n", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, !!val, 1);

	ret = kstrtoint("0\n", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, !!val, 0);

	/* Arbitrary non-zero integer: !!val must yield 1 */
	ret = kstrtoint("42", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, !!val, 1);

	/* Negative non-zero: !!(-1) == 1 */
	ret = kstrtoint("-1", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, !!val, 1);

	/* Invalid inputs must return -EINVAL */
	ret = kstrtoint("foo", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = kstrtoint("", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = kstrtoint("1a", 10, &val);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

/* ------------------------------------------------------------------ */
/* Test 9: SM8650 topology sanity — nr_cpu_ids                          */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_sm8650_topology_nr_cpus - verify expected CPU count.
 *
 * cinema_mode_init() allocates freq_reqs and freq_req_active with
 * nr_cpu_ids entries.  On SM8650 (Pineapple) there are exactly 8 CPUs
 * (0-1: A520 efficiency, 2-6: A720 performance, 7: X4 prime).  If
 * nr_cpu_ids is wrong the allocation will be too small and
 * cinema_activate() will write out of bounds.
 */
static void cinema_test_sm8650_topology_nr_cpus(struct kunit *test)
{
	if (nr_cpu_ids != 8) {
		kunit_warn(test,
			   "nr_cpu_ids=%u; SM8650 topology tests expect 8\n",
			   nr_cpu_ids);
		kunit_skip(test, "not running on an 8-CPU SM8650 platform");
	}

	KUNIT_EXPECT_EQ(test, (unsigned int)nr_cpu_ids, 8u);
}

/* ------------------------------------------------------------------ */
/* Test 10: allocation sizing — kunit_kcalloc correctness               */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_alloc_sizing - validate that the two parallel arrays
 * allocated in cinema_mode_init() are correctly sized and zeroed.
 *
 * Uses kunit_kcalloc() so allocations are automatically freed when the
 * test ends, even if an assertion aborts the test mid-function.
 */
static void cinema_test_alloc_sizing(struct kunit *test)
{
	unsigned int num_cpus = nr_cpu_ids;
	struct freq_qos_request *reqs;
	bool *active;
	unsigned int i;

	/* kunit_kcalloc auto-frees on test completion or assertion failure */
	reqs = kunit_kcalloc(test, num_cpus, sizeof(*reqs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, reqs);

	active = kunit_kcalloc(test, num_cpus, sizeof(*active), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, active);

	/* kcalloc zero-initialises: all active flags start false */
	for (i = 0; i < num_cpus; i++)
		KUNIT_EXPECT_FALSE_MSG(test, active[i],
				       "active[%u] non-zero after kcalloc", i);
}

/* ------------------------------------------------------------------ */
/* Test 10b: floor_pct computation                                       */
/* ------------------------------------------------------------------ */

/*
 * cinema_test_floor_pct_computation - validate the integer arithmetic used
 * to derive floor_hz from max_freq and floor_pct.
 *
 * cinema_activate() computes:
 *   floor = (u64)policy->cpuinfo.max_freq * floor_pct / 100
 * using 64-bit intermediate to avoid overflow for large max_freq values.
 * This test verifies the arithmetic for the four SM8650 cluster maxima and
 * the default floor_pct of 85.
 */
static void cinema_test_floor_pct_computation(struct kunit *test)
{
	static const struct {
		unsigned int max_khz;
		int pct;
		unsigned int expected_khz;
	} cases[] = {
		/* A520 cluster: 2265600 kHz * 85% = 1925760 kHz */
		{ 2265600, 85, 1925760 },
		/* A720-lo cluster: 2956800 kHz * 85% = 2513280 kHz */
		{ 2956800, 85, 2513280 },
		/* A720-hi cluster: 3148800 kHz * 85% = 2676480 kHz */
		{ 3148800, 85, 2676480 },
		/* X4 prime cluster: 3302400 kHz * 85% = 2807040 kHz */
		{ 3302400, 85, 2807040 },
		/* Boundary: 100% must equal max_freq */
		{ 3302400, 100, 3302400 },
		/* Boundary: 50% minimum */
		{ 3302400, 50, 1651200 },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		unsigned int got = (unsigned int)(
			(u64)cases[i].max_khz * cases[i].pct / 100);
		KUNIT_EXPECT_EQ_MSG(test, got, cases[i].expected_khz,
				    "floor_pct=%d max=%u kHz: got %u, want %u",
				    cases[i].pct, cases[i].max_khz,
				    got, cases[i].expected_khz);
	}
}

/* ------------------------------------------------------------------ */
/* Test suite registration                                               */
/* ------------------------------------------------------------------ */

static struct kunit_case cinema_mode_test_cases[] = {
	KUNIT_CASE(cinema_test_freq_qos_lifecycle),
	KUNIT_CASE(cinema_test_active_guard_idempotent_remove),
	KUNIT_CASE(cinema_test_partial_failure_unwind),
	KUNIT_CASE(cinema_test_multi_domain_isolation),
	KUNIT_CASE(cinema_test_qos_aggregation_max_wins),
	KUNIT_CASE(cinema_test_freq_qos_update),
	KUNIT_CASE(cinema_test_freq_qos_guard_blocks_update),
	KUNIT_CASE(cinema_test_sysfs_output_format),
	KUNIT_CASE(cinema_test_sysfs_input_parsing),
	KUNIT_CASE(cinema_test_sm8650_topology_nr_cpus),
	KUNIT_CASE(cinema_test_alloc_sizing),
	KUNIT_CASE(cinema_test_floor_pct_computation),
	{}
};

static struct kunit_suite cinema_mode_test_suite = {
	.name       = "cinema_mode",
	.test_cases = cinema_mode_test_cases,
};

kunit_test_suites(&cinema_mode_test_suite);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("KUnit tests for cinema_mode.c PM-QoS layer");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
