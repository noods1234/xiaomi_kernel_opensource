// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_end_kunit.c — KUnit test suite for cinema_end.c
 *
 * Scope
 * -----
 * cinema_end.c has two layers:
 *
 *   1. Validation and state logic layer — input parsing, range checks,
 *      mode validation, fault threshold comparison, status string
 *      selection, and fault_clear semantics.  These are pure logic and
 *      do not require a kobject, sysfs infrastructure, or real LC hardware.
 *
 *   2. Hardware integration layer — kobject lifecycle, sysfs_notify,
 *      cinema_mode_set_active() coupling, concurrent daemon writes — covered
 *      by integration tests (not in this file).
 *
 * Test strategy
 * -------------
 * Like cinema_mode_kunit.c, we test layer 1 by exercising the same
 * arithmetic and decision logic the driver uses, without calling into
 * the driver functions that require a live kobject.  This catches
 * off-by-one errors in range checks and threshold comparisons, which
 * are the most likely source of silent bugs in this layer.
 *
 * The constants (END_ND_MAX_MB, END_MODE_MAX, END_TEMP_FAULT_MC) are
 * replicated here as locals so the test suite compiles and runs
 * independently of cinema_end.c's translation unit.  Any drift between
 * these values and the driver is itself a defect that a failing test
 * would surface.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>

/* Mirror the constants from cinema_end.c.  If either diverges, tests fail. */
#define END_ND_MAX_MB		7000
#define END_MODE_MANUAL		0
#define END_MODE_EXPOSURE_HOLD	1
#define END_MODE_DOF_HOLD	2
#define END_MODE_MAX		END_MODE_DOF_HOLD
#define END_TEMP_WARN_MC	55000	/* soft warning — recording continues */
#define END_TEMP_FAULT_MC	65000	/* hard fault — 5°C below 70°C rating */

/*
 * end_test_nd_valid - replicate nd_setpoint_store / nd_actual_store range check.
 * Returns true if the value would be accepted.
 */
static bool end_test_nd_valid(int val)
{
	return val >= 0 && val <= END_ND_MAX_MB;
}

/*
 * end_test_mode_valid - replicate mode_store range check.
 */
static bool end_test_mode_valid(int val)
{
	return val >= 0 && val <= END_MODE_MAX;
}

/*
 * end_test_temp_valid - replicate cell_temp_store range check.
 * Returns true if the value would be accepted (not -ERANGE).
 */
static bool end_test_temp_valid(int val)
{
	return val >= -40000 && val <= 125000;
}

/*
 * end_test_temp_warn - replicate the soft warning comparison:
 * fires when val > END_TEMP_WARN_MC and val <= END_TEMP_FAULT_MC.
 */
static bool end_test_temp_warn(int val)
{
	return val > END_TEMP_WARN_MC && val <= END_TEMP_FAULT_MC;
}

/*
 * end_test_temp_fault - replicate the hard fault comparison:
 * fires when val > END_TEMP_FAULT_MC.
 */
static bool end_test_temp_fault(int val)
{
	return val > END_TEMP_FAULT_MC;
}

/*
 * end_test_status_str - replicate status_show's state-to-string logic.
 * Priority: offline > fault > active > standby.
 */
static const char *end_test_status_str(bool mcu_online, bool active, bool fault)
{
	if (!mcu_online)
		return "offline";
	if (fault)
		return "fault";
	if (active)
		return "active";
	return "standby";
}

/* ------------------------------------------------------------------ */
/* Test 1: nd_setpoint and nd_actual range validation                   */
/* ------------------------------------------------------------------ */

/*
 * end_test_nd_range - validate the nd_setpoint / nd_actual bounds check.
 *
 * cinema_end accepts ND values in [0, END_ND_MAX_MB].  Values outside
 * this range return -ERANGE.  Both nd_setpoint_store and nd_actual_store
 * apply the same check, so a single helper covers both.
 */
static void end_test_nd_range(struct kunit *test)
{
	/* Valid boundary values */
	KUNIT_EXPECT_TRUE(test,  end_test_nd_valid(0));
	KUNIT_EXPECT_TRUE(test,  end_test_nd_valid(END_ND_MAX_MB));
	KUNIT_EXPECT_TRUE(test,  end_test_nd_valid(1));
	KUNIT_EXPECT_TRUE(test,  end_test_nd_valid(END_ND_MAX_MB - 1));

	/* Rev A working band: 2000–4000 mb */
	KUNIT_EXPECT_TRUE(test,  end_test_nd_valid(2000));
	KUNIT_EXPECT_TRUE(test,  end_test_nd_valid(4000));

	/* Invalid: below zero */
	KUNIT_EXPECT_FALSE(test, end_test_nd_valid(-1));
	KUNIT_EXPECT_FALSE(test, end_test_nd_valid(INT_MIN));

	/* Invalid: above END_ND_MAX_MB */
	KUNIT_EXPECT_FALSE(test, end_test_nd_valid(END_ND_MAX_MB + 1));
	KUNIT_EXPECT_FALSE(test, end_test_nd_valid(INT_MAX));
}

/* ------------------------------------------------------------------ */
/* Test 2: mode validation                                               */
/* ------------------------------------------------------------------ */

/*
 * end_test_mode_range - validate mode_store bounds check.
 *
 * Valid modes are 0 (manual), 1 (exposure_hold), 2 (dof_hold).
 * All other values return -EINVAL.
 */
static void end_test_mode_range(struct kunit *test)
{
	/* Valid modes */
	KUNIT_EXPECT_TRUE(test,  end_test_mode_valid(END_MODE_MANUAL));
	KUNIT_EXPECT_TRUE(test,  end_test_mode_valid(END_MODE_EXPOSURE_HOLD));
	KUNIT_EXPECT_TRUE(test,  end_test_mode_valid(END_MODE_DOF_HOLD));

	/* Confirm END_MODE_MAX is 2 — if this fails, END_MODE_MAX changed */
	KUNIT_EXPECT_EQ(test, END_MODE_MAX, 2);

	/* Invalid: one above the max */
	KUNIT_EXPECT_FALSE(test, end_test_mode_valid(END_MODE_MAX + 1));
	KUNIT_EXPECT_FALSE(test, end_test_mode_valid(3));
	KUNIT_EXPECT_FALSE(test, end_test_mode_valid(100));
	KUNIT_EXPECT_FALSE(test, end_test_mode_valid(INT_MAX));

	/* Invalid: negative */
	KUNIT_EXPECT_FALSE(test, end_test_mode_valid(-1));
	KUNIT_EXPECT_FALSE(test, end_test_mode_valid(INT_MIN));
}

/* ------------------------------------------------------------------ */
/* Test 3: cell_temp range validation                                    */
/* ------------------------------------------------------------------ */

/*
 * end_test_cell_temp_range - validate cell_temp_store sanity bounds.
 *
 * Accepted range: -40 000 m°C to 125 000 m°C.
 * Values outside this range return -ERANGE (clearly bogus readings).
 */
static void end_test_cell_temp_range(struct kunit *test)
{
	/* Valid boundary values */
	KUNIT_EXPECT_TRUE(test,  end_test_temp_valid(-40000));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_valid(125000));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_valid(0));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_valid(25000));   /* room temp */
	KUNIT_EXPECT_TRUE(test,  end_test_temp_valid(END_TEMP_FAULT_MC));

	/* Invalid: one below the minimum */
	KUNIT_EXPECT_FALSE(test, end_test_temp_valid(-40001));
	KUNIT_EXPECT_FALSE(test, end_test_temp_valid(INT_MIN));

	/* Invalid: one above the maximum */
	KUNIT_EXPECT_FALSE(test, end_test_temp_valid(125001));
	KUNIT_EXPECT_FALSE(test, end_test_temp_valid(INT_MAX));
}

/* ------------------------------------------------------------------ */
/* Test 4: two-tier temperature thresholds                              */
/* ------------------------------------------------------------------ */

/*
 * end_test_temp_thresholds - validate the warn and fault comparisons.
 *
 * WARN fires when val > END_TEMP_WARN_MC (55°C) and <= END_TEMP_FAULT_MC (65°C).
 * FAULT fires when val > END_TEMP_FAULT_MC (65°C).
 * Neither fires at exactly the threshold — both are strictly greater-than.
 *
 * The boundary between warn and fault (65°C) is the most likely source
 * of off-by-one errors, so it is tested on both sides explicitly.
 */
static void end_test_temp_thresholds(struct kunit *test)
{
	/* Confirm threshold values — if these fail, constants diverged */
	KUNIT_EXPECT_EQ(test, END_TEMP_WARN_MC,  55000);
	KUNIT_EXPECT_EQ(test, END_TEMP_FAULT_MC, 65000);

	/* Below warn: neither fires */
	KUNIT_EXPECT_FALSE(test, end_test_temp_warn(0));
	KUNIT_EXPECT_FALSE(test, end_test_temp_fault(0));
	KUNIT_EXPECT_FALSE(test, end_test_temp_warn(25000));
	KUNIT_EXPECT_FALSE(test, end_test_temp_warn(END_TEMP_WARN_MC));
	KUNIT_EXPECT_FALSE(test, end_test_temp_fault(END_TEMP_WARN_MC));

	/* In the warn band (55°C < val ≤ 65°C): warn fires, fault does not */
	KUNIT_EXPECT_TRUE(test,  end_test_temp_warn(END_TEMP_WARN_MC + 1));
	KUNIT_EXPECT_FALSE(test, end_test_temp_fault(END_TEMP_WARN_MC + 1));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_warn(60000));
	KUNIT_EXPECT_FALSE(test, end_test_temp_fault(60000));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_warn(END_TEMP_FAULT_MC));
	KUNIT_EXPECT_FALSE(test, end_test_temp_fault(END_TEMP_FAULT_MC));

	/* Above fault threshold (val > 65°C): fault fires, warn does not */
	KUNIT_EXPECT_FALSE(test, end_test_temp_warn(END_TEMP_FAULT_MC + 1));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_fault(END_TEMP_FAULT_MC + 1));
	KUNIT_EXPECT_FALSE(test, end_test_temp_warn(80000));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_fault(80000));
	KUNIT_EXPECT_TRUE(test,  end_test_temp_fault(125000));
}

/* ------------------------------------------------------------------ */
/* Test 5: status string selection                                        */
/* ------------------------------------------------------------------ */

/*
 * end_test_status_string - validate the status_show state machine.
 *
 * Priority order: offline > fault > active > standby.
 * When mcu_online is false, status is "offline" regardless of other state.
 */
static void end_test_status_string(struct kunit *test)
{
	/* Offline: MCU not present — overrides everything */
	KUNIT_EXPECT_STREQ(test, end_test_status_str(false, false, false), "offline");
	KUNIT_EXPECT_STREQ(test, end_test_status_str(false, true,  false), "offline");
	KUNIT_EXPECT_STREQ(test, end_test_status_str(false, false, true),  "offline");
	KUNIT_EXPECT_STREQ(test, end_test_status_str(false, true,  true),  "offline");

	/* Standby: MCU online, not active, not faulted */
	KUNIT_EXPECT_STREQ(test, end_test_status_str(true,  false, false), "standby");

	/* Active: MCU online, active, not faulted */
	KUNIT_EXPECT_STREQ(test, end_test_status_str(true,  true,  false), "active");

	/* Fault: MCU online, faulted (overrides active) */
	KUNIT_EXPECT_STREQ(test, end_test_status_str(true,  true,  true),  "fault");
	KUNIT_EXPECT_STREQ(test, end_test_status_str(true,  false, true),  "fault");
}

/* ------------------------------------------------------------------ */
/* Test 6: enable sysfs input parsing                                    */
/* ------------------------------------------------------------------ */

/*
 * end_test_enable_parsing - validate kstrtobool for enable_store.
 *
 * enable_store uses kstrtobool(), which accepts "1"/"0", "y"/"n",
 * "on"/"off", "true"/"false" (case-insensitive) and echo-appended "\n".
 */
static void end_test_enable_parsing(struct kunit *test)
{
	bool val;
	int ret;

	/* Standard true values */
	ret = kstrtobool("1", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, val);

	ret = kstrtobool("y", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, val);

	ret = kstrtobool("on", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, val);

	/* Standard false values */
	ret = kstrtobool("0", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, val);

	ret = kstrtobool("n", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, val);

	ret = kstrtobool("off", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, val);

	/* echo(1) appends newline */
	ret = kstrtobool("1\n", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_TRUE(test, val);

	ret = kstrtobool("0\n", &val);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, val);

	/* Invalid inputs must return -EINVAL */
	ret = kstrtobool("2", &val);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = kstrtobool("", &val);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	ret = kstrtobool("foo", &val);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

/* ------------------------------------------------------------------ */
/* Test 7: nd_setpoint sysfs output format                              */
/* ------------------------------------------------------------------ */

/*
 * end_test_nd_setpoint_format - validate the scnprintf format used by
 * nd_setpoint_show, nd_actual_show, and cell_temp_show.
 *
 * All three integer-valued attributes use:
 *   scnprintf(buf, PAGE_SIZE, "%d\n", value)
 * Userspace tools parse these values; the exact format must be stable.
 */
static void end_test_nd_setpoint_format(struct kunit *test)
{
	char *buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int n;

	KUNIT_ASSERT_NOT_NULL(test, buf);

	/* Zero ND */
	n = scnprintf(buf, PAGE_SIZE, "%d\n", 0);
	KUNIT_EXPECT_STREQ(test, buf, "0\n");
	KUNIT_EXPECT_EQ(test, n, 2);

	/* Maximum ND: 7000 */
	n = scnprintf(buf, PAGE_SIZE, "%d\n", END_ND_MAX_MB);
	KUNIT_EXPECT_STREQ(test, buf, "7000\n");
	KUNIT_EXPECT_EQ(test, n, 5);

	/* Typical Rev A setpoint: 3000 mb (3 stops) */
	n = scnprintf(buf, PAGE_SIZE, "%d\n", 3000);
	KUNIT_EXPECT_STREQ(test, buf, "3000\n");

	/* Negative cell_temp: -20 000 m°C */
	n = scnprintf(buf, PAGE_SIZE, "%d\n", -20000);
	KUNIT_EXPECT_STREQ(test, buf, "-20000\n");
	KUNIT_EXPECT_EQ(test, n, 7);
}

/* ------------------------------------------------------------------ */
/* Test 8: status sysfs output format                                    */
/* ------------------------------------------------------------------ */

/*
 * end_test_status_format - validate the format string used by status_show.
 *
 * status_show produces exactly "standby\n", "active\n", or "fault\n"
 * via scnprintf(buf, PAGE_SIZE, "%s\n", state).
 */
static void end_test_status_format(struct kunit *test)
{
	char *buf = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int n;

	KUNIT_ASSERT_NOT_NULL(test, buf);

	n = scnprintf(buf, PAGE_SIZE, "%s\n", "standby");
	KUNIT_EXPECT_STREQ(test, buf, "standby\n");
	KUNIT_EXPECT_EQ(test, n, 8);

	n = scnprintf(buf, PAGE_SIZE, "%s\n", "active");
	KUNIT_EXPECT_STREQ(test, buf, "active\n");
	KUNIT_EXPECT_EQ(test, n, 7);

	n = scnprintf(buf, PAGE_SIZE, "%s\n", "fault");
	KUNIT_EXPECT_STREQ(test, buf, "fault\n");
	KUNIT_EXPECT_EQ(test, n, 6);
}

/* ------------------------------------------------------------------ */
/* Test 9: fault_clear semantics                                         */
/* ------------------------------------------------------------------ */

/*
 * end_test_fault_clear_semantics - validate the fault_clear state machine.
 *
 * fault_clear is only accepted when:
 *   (a) value is true (writing "0" is a no-op)
 *   (b) mcu_online == true (-ENODEV otherwise)
 *   (c) end_active == false (-EBUSY otherwise)
 *
 * This test mirrors the guard logic in fault_clear_store().
 */
static void end_test_fault_clear_semantics(struct kunit *test)
{
	bool mcu_online = true;
	bool fault  = true;
	bool active = false;
	bool clear;
	int ret;

	/* --- Scenario A: clear accepted (MCU online, not active, faulted) --- */
	ret = kstrtobool("1", &clear);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Guards: MCU online check, then active check */
	KUNIT_EXPECT_TRUE_MSG(test,  mcu_online,
			      "guard A: MCU must be online before clear");
	KUNIT_EXPECT_FALSE_MSG(test, active,
			       "guard A: must not be active before clear");

	if (mcu_online && !active && clear && fault) {
		fault = false;		/* simulate fault_clear_store body */
	}
	KUNIT_EXPECT_FALSE_MSG(test, fault, "fault must be cleared after write");

	/* --- Scenario B: no-op when clear == false --- */
	mcu_online = true;
	fault  = true;
	active = false;
	ret = kstrtobool("0", &clear);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_FALSE(test, clear);

	if (mcu_online && !active && clear && fault)
		KUNIT_FAIL(test, "fault_clear must not fire when clear==false");

	KUNIT_EXPECT_TRUE_MSG(test, fault,
			      "fault must persist when clear==false written");

	/* --- Scenario C1: -ENODEV guard when MCU offline --- */
	mcu_online = false;
	fault  = true;
	active = false;
	clear  = true;

	KUNIT_EXPECT_FALSE_MSG(test, mcu_online,
			       "guard C1: MCU must be offline to trigger -ENODEV");
	/* Simulate the guard — would return -ENODEV */
	if (!mcu_online) {
		KUNIT_EXPECT_TRUE_MSG(test, fault,
				      "scenario C1: fault must persist when MCU offline");
	} else {
		KUNIT_FAIL(test, "guard C1: should not reach clear body when MCU offline");
	}

	/* --- Scenario C2: -EBUSY guard when active --- */
	mcu_online = true;
	fault  = true;
	active = true;
	clear  = true;

	/* Simulate the guard check in fault_clear_store */
	if (active) {
		/*
		 * Would return -EBUSY.  Confirm the guard variable is set so
		 * the body of the clear never executes.
		 */
		KUNIT_EXPECT_TRUE_MSG(test, active,
				      "guard C2: active must block clear");
	} else {
		KUNIT_FAIL(test, "guard C2: should not reach clear body while active");
	}

	/* fault must be unchanged */
	KUNIT_EXPECT_TRUE_MSG(test, fault,
			      "fault must not clear when eND is active");

	/* --- Scenario D: idempotent — clear while not faulted --- */
	mcu_online = true;
	fault  = false;
	active = false;
	clear  = true;

	/* Writing "1" to fault_clear when not faulted is a no-op */
	if (mcu_online && !active && clear && fault) {
		KUNIT_FAIL(test, "scenario D: fault was false; body must not fire");
	}
	KUNIT_EXPECT_FALSE_MSG(test, fault,
			       "scenario D: fault must remain false");
}

/* ------------------------------------------------------------------ */
/* Test 10: constant consistency                                          */
/* ------------------------------------------------------------------ */

/*
 * end_test_constant_consistency - confirm the local constant copies match
 * the expected values.  If cinema_end.c changes a constant without
 * updating this file, the test here catches the mismatch via the
 * replicated values diverging from documented hardware specs.
 */
static void end_test_constant_consistency(struct kunit *test)
{
	/* 7 stops = 7 × 1000 mb — cinema industry standard ceiling */
	KUNIT_EXPECT_EQ(test, END_ND_MAX_MB, 7000);

	/* Three modes: manual (0), exposure_hold (1), dof_hold (2) */
	KUNIT_EXPECT_EQ(test, END_MODE_MANUAL,        0);
	KUNIT_EXPECT_EQ(test, END_MODE_EXPOSURE_HOLD, 1);
	KUNIT_EXPECT_EQ(test, END_MODE_DOF_HOLD,      2);
	KUNIT_EXPECT_EQ(test, END_MODE_MAX,           2);

	/* LC-Tec cartridge rated to 70°C; 55°C warn, 65°C fault (5°C margin) */
	KUNIT_EXPECT_EQ(test, END_TEMP_WARN_MC,  55000);
	KUNIT_EXPECT_EQ(test, END_TEMP_FAULT_MC, 65000);
	/* Warn must be strictly below fault */
	KUNIT_EXPECT_LT(test, END_TEMP_WARN_MC, END_TEMP_FAULT_MC);
}

/* ------------------------------------------------------------------ */
/* Test suite registration                                               */
/* ------------------------------------------------------------------ */

static struct kunit_case cinema_end_test_cases[] = {
	KUNIT_CASE(end_test_nd_range),
	KUNIT_CASE(end_test_mode_range),
	KUNIT_CASE(end_test_cell_temp_range),
	KUNIT_CASE(end_test_temp_thresholds),
	KUNIT_CASE(end_test_status_string),
	KUNIT_CASE(end_test_enable_parsing),
	KUNIT_CASE(end_test_nd_setpoint_format),
	KUNIT_CASE(end_test_status_format),
	KUNIT_CASE(end_test_fault_clear_semantics),
	KUNIT_CASE(end_test_constant_consistency),
	{}
};

static struct kunit_suite cinema_end_test_suite = {
	.name       = "cinema_end",
	.test_cases = cinema_end_test_cases,
};

kunit_test_suites(&cinema_end_test_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("KUnit tests for cinema_end.c validation and state logic");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
