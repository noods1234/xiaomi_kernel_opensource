// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_end.c — Electronic neutral density (eND) control interface
 *
 * For Xiaomi 14 Ultra (aurora) / SM8650 (Pineapple).
 *
 * Hardware: LC-Tec PolarView eND(NBf2.0) dual-cell guest-host cartridge,
 * driven by an STM32U5 + AD5696R + TPS65131 sidecar board via USB/UART.
 *
 * This driver owns the kernel-side control and telemetry interface.
 * The actual LC drive waveform generation is performed by a privileged
 * userspace daemon (cinema_end_daemon) that bridges between this sysfs
 * interface and the MCU over USB-CDC or UART.
 *
 * Kernel responsibilities:
 *   - sysfs interface for Android cinema app → MCU command path
 *   - sysfs telemetry sink for MCU daemon → kernel → userspace path
 *   - ND setpoint validation and slew-rate accounting (coarse, frame-level)
 *   - State interlock with cinema_mode performance coordinator
 *
 * Userspace (daemon) responsibilities:
 *   - USB/UART framing and MCU protocol
 *   - AC drive waveform generation (via MCU)
 *   - Temperature-aware LUT interpolation
 *   - Closed-loop ND feedback
 *   - Flicker-guard (drive frequency vs frame rate)
 *
 * Sysfs nodes (under /sys/kernel/cinema_end/):
 *
 *   enable       rw  0 = standby, 1 = active.  Requires CAP_SYS_ADMIN.
 *                    Writing 1 also activates the cinema_mode performance
 *                    coordinator via cinema_mode_set_active(true).
 *
 *   nd_setpoint  rw  Target ND value in millibels (thousandths of one stop).
 *                    Range: 0 (clear) to END_ND_MAX_MB (7 stops = 7000 mb).
 *                    Rev A working band: 2000–4000 mb (2–4 stops).
 *                    Requires CAP_SYS_ADMIN to write.
 *                    Written by Android cinema app or Camera2 AE feedback.
 *
 *   mode         rw  Operating mode:
 *                      0 = END_MODE_MANUAL       (operator sets nd_setpoint)
 *                      1 = END_MODE_EXPOSURE_HOLD (ND tracks AE, fixed shutter)
 *                      2 = END_MODE_DOF_HOLD      (ND compensates for iris)
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   nd_actual    rw  Current achieved ND in millibels, as reported by the MCU.
 *                    Written by the userspace daemon from MCU telemetry.
 *                    Read by the cinema app for UI display.
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   cell_temp    rw  LC cell temperature in millidegrees Celsius, from TMP117
 *                    on the cartridge flex tail.  Written by the daemon.
 *                    Triggers a fault if temperature exceeds END_TEMP_FAULT_MC.
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   status       ro  Human-readable state string: "standby", "active",
 *                    or "fault".  Reflects end_active and end_fault flags.
 *
 *   fault_clear  wo  Write "1" after MCU recovery to clear fault state.
 *                    Only accepted when end_active == false (-EBUSY otherwise).
 *                    Writing "0" is a no-op.  Requires CAP_SYS_ADMIN.
 *
 * Integration with cinema_mode
 * ----------------------------
 * Enabling eND also enables the cinema_mode performance coordinator
 * (CPU frequency floor, deep-idle block, suspend prevention) via the
 * exported cinema_mode_set_active() API.  Disabling eND or entering fault
 * state also releases the performance coordinator.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

/* Maximum representable ND in millibels.  7 stops covers Sony's full
 * public cinema eND range with margin.  Rev A working band is 2000–4000.
 */
#define END_ND_MAX_MB		7000

/* Operating modes */
#define END_MODE_MANUAL		0  /* operator sets nd_setpoint directly */
#define END_MODE_EXPOSURE_HOLD	1  /* ND tracks AE, shutter angle fixed */
#define END_MODE_DOF_HOLD	2  /* ND compensates when iris changes */
#define END_MODE_MAX		END_MODE_DOF_HOLD

/*
 * Temperature fault threshold: 55 000 m°C (55°C).
 * The LC cell and MCU sidecar board are rated to 85°C, but 55°C is the
 * conservative operating limit for the LC-Tec cartridge under active drive.
 * The TMP117 hardware alert fires first; this threshold is a software
 * second-line-of-defence that lets the cinema app react without polling.
 */
#define END_TEMP_FAULT_MC	55000

/* ------------------------------------------------------------------ */
/* Inter-module API (from cinema_mode.c)                                */
/* ------------------------------------------------------------------ */

int cinema_mode_set_active(bool on);

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static bool end_active;		/* eND hardware enabled */
static bool end_fault;		/* MCU or hardware fault signalled */
static int  end_nd_setpoint;	/* target ND in millibels */
static int  end_mode;		/* END_MODE_* */
static int  end_nd_actual;	/* last MCU-reported actual ND, millibels */
static int  end_cell_temp;	/* last MCU-reported cell temp, m°C */

static struct kobject *end_kobj;
static DEFINE_MUTEX(end_lock);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * end_enter_fault - transition to fault state under end_lock.
 *
 * Sets end_fault, deactivates eND, and releases the cinema_mode
 * performance coordinator.  Notifies sysfs waiters on "status".
 * Must be called with end_lock held.
 */
static void end_enter_fault(const char *reason)
{
	if (end_fault)
		return;	/* already faulted */

	end_fault  = true;
	end_active = false;

	/*
	 * Release the performance coordinator.  cinema_mode_set_active()
	 * acquires cinema_lock internally; we must not hold it here.
	 * end_lock and cinema_lock have a strict order:
	 *   end_lock → cinema_lock (always; never the reverse).
	 * This call is therefore safe.
	 */
	cinema_mode_set_active(false);

	pr_warn("cinema_end: fault — %s\n", reason);

	/*
	 * Notify userspace.  sysfs_notify() can be called from any context
	 * while holding end_lock (it uses its own internal spinlock).
	 */
	sysfs_notify(end_kobj, NULL, "status");
}

/* ------------------------------------------------------------------ */
/* enable                                                               */
/* ------------------------------------------------------------------ */

static ssize_t enable_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	bool active;

	mutex_lock(&end_lock);
	active = end_active;
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", active ? 1 : 0);
}

static ssize_t enable_store(struct kobject *kobj,
			    struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	bool on;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtobool(buf, &on))
		return -EINVAL;

	mutex_lock(&end_lock);

	if (on == end_active)
		goto out;

	if (on && end_fault) {
		/* Cannot re-enable while in fault state */
		ret = -ENODEV;
		goto out;
	}

	end_active = on;
	if (!on)
		end_fault = false;	/* clear fault on explicit disable */

	/*
	 * Couple eND enable/disable to the cinema_mode performance
	 * coordinator.  cinema_mode_set_active() acquires cinema_lock;
	 * end_lock → cinema_lock order is always observed (see end_enter_fault).
	 */
	ret = cinema_mode_set_active(on);
	if (ret && on) {
		/* Performance coordinator failed; roll back eND enable */
		end_active = false;
		goto out;
	}

	pr_debug("cinema_end: eND state %s\n", on ? "active" : "standby");

	/* Notify userspace of state change */
	sysfs_notify(end_kobj, NULL, "status");

out:
	mutex_unlock(&end_lock);
	return ret ? ret : count;
}

/* ------------------------------------------------------------------ */
/* nd_setpoint                                                          */
/* ------------------------------------------------------------------ */

static ssize_t nd_setpoint_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	int sp;

	mutex_lock(&end_lock);
	sp = end_nd_setpoint;
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", sp);
}

static ssize_t nd_setpoint_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > END_ND_MAX_MB)
		return -ERANGE;

	mutex_lock(&end_lock);
	end_nd_setpoint = val;
	mutex_unlock(&end_lock);

	/* Wake any daemon poll()ing on this node for setpoint changes. */
	sysfs_notify(end_kobj, NULL, "nd_setpoint");

	return count;
}

/* ------------------------------------------------------------------ */
/* mode                                                                 */
/* ------------------------------------------------------------------ */

static ssize_t mode_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	int mode;

	mutex_lock(&end_lock);
	mode = end_mode;
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t mode_store(struct kobject *kobj,
			  struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > END_MODE_MAX)
		return -EINVAL;

	mutex_lock(&end_lock);
	end_mode = val;
	mutex_unlock(&end_lock);

	pr_debug("cinema_end: mode → %d (%s)\n", val,
		 val == END_MODE_MANUAL        ? "manual"        :
		 val == END_MODE_EXPOSURE_HOLD ? "exposure_hold" :
						 "dof_hold");

	return count;
}

/* ------------------------------------------------------------------ */
/* nd_actual (daemon telemetry write-back)                              */
/* ------------------------------------------------------------------ */

static ssize_t nd_actual_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	int actual;

	mutex_lock(&end_lock);
	actual = end_nd_actual;
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", actual);
}

static ssize_t nd_actual_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	int val;

	/* Only the privileged MCU bridge daemon updates this field */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	if (val < 0 || val > END_ND_MAX_MB)
		return -ERANGE;

	mutex_lock(&end_lock);
	end_nd_actual = val;
	mutex_unlock(&end_lock);

	return count;
}

/* ------------------------------------------------------------------ */
/* cell_temp (daemon telemetry write-back)                              */
/* ------------------------------------------------------------------ */

static ssize_t cell_temp_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	int temp;

	mutex_lock(&end_lock);
	temp = end_cell_temp;
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", temp);
}

static ssize_t cell_temp_store(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       const char *buf, size_t count)
{
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	/*
	 * Sanity bounds: −40°C (−40 000 m°C) to 125°C (125 000 m°C).
	 * TMP117 hardware range is −55°C to +150°C but the LC cell and
	 * MCU are rated to 85°C max; reject anything clearly bogus.
	 */
	if (val < -40000 || val > 125000)
		return -ERANGE;

	mutex_lock(&end_lock);

	end_cell_temp = val;

	/*
	 * Temperature fault detection: if the cell exceeds the operating
	 * limit (END_TEMP_FAULT_MC = 55°C), enter fault state.  The TMP117
	 * hardware alert fires first; this is the kernel-side second net that
	 * allows the cinema app to react via a status poll without a daemon
	 * round-trip.
	 *
	 * end_enter_fault() releases the performance coordinator and notifies
	 * sysfs waiters.  It is a no-op if already faulted.
	 */
	if (val > END_TEMP_FAULT_MC && end_active)
		end_enter_fault("cell temperature exceeded 55°C limit");

	mutex_unlock(&end_lock);

	return count;
}

/* ------------------------------------------------------------------ */
/* status (read-only state summary)                                     */
/* ------------------------------------------------------------------ */

static ssize_t status_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	const char *state;

	mutex_lock(&end_lock);
	if (end_fault)
		state = "fault";
	else if (end_active)
		state = "active";
	else
		state = "standby";
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%s\n", state);
}

/* ------------------------------------------------------------------ */
/* fault_clear (write-only daemon recovery signal)                      */
/* ------------------------------------------------------------------ */

/*
 * fault_clear_store - clear the fault state after MCU recovery.
 *
 * The typical fault recovery sequence is:
 *   1. Kernel detects over-temperature → end_enter_fault() fires,
 *      status → "fault", cinema_mode deactivated.
 *   2. MCU daemon notices status == "fault" via poll()/read.
 *   3. MCU cools down; daemon signals kernel recovery by writing "1"
 *      to fault_clear.
 *   4. Kernel clears end_fault; status → "standby".
 *   5. Camera app may now re-enable via enable_store("1").
 *
 * Invariant: fault_clear is only accepted when eND is in standby
 * (end_active == false).  The app must explicitly disable eND before
 * the daemon can clear the fault — this prevents a spurious clear from
 * racing with an active recording session.
 *
 * Writing "0" is a no-op.  Writing while end_active returns -EBUSY.
 * If not currently faulted, writing "1" is a no-op (idempotent).
 */
static ssize_t fault_clear_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	bool clear;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtobool(buf, &clear))
		return -EINVAL;

	if (!clear)
		return count;	/* writing "0" is a no-op */

	mutex_lock(&end_lock);

	if (end_active) {
		mutex_unlock(&end_lock);
		return -EBUSY;	/* must disable eND before clearing fault */
	}

	if (end_fault) {
		end_fault = false;
		pr_info("cinema_end: fault cleared by daemon\n");
		sysfs_notify(end_kobj, NULL, "status");
	}

	mutex_unlock(&end_lock);
	return count;
}

static struct kobj_attribute fault_clear_attr =
	__ATTR(fault_clear, 0200, NULL, fault_clear_store);

/* ------------------------------------------------------------------ */
/* Attribute wiring                                                     */
/* ------------------------------------------------------------------ */

static struct kobj_attribute enable_attr =
	__ATTR(enable,      0600, enable_show,      enable_store);
static struct kobj_attribute nd_setpoint_attr =
	__ATTR(nd_setpoint, 0600, nd_setpoint_show, nd_setpoint_store);
static struct kobj_attribute mode_attr =
	__ATTR(mode,        0600, mode_show,        mode_store);
static struct kobj_attribute nd_actual_attr =
	__ATTR(nd_actual,   0600, nd_actual_show,   nd_actual_store);
static struct kobj_attribute cell_temp_attr =
	__ATTR(cell_temp,   0600, cell_temp_show,   cell_temp_store);
static struct kobj_attribute status_attr =
	__ATTR(status,      0444, status_show,      NULL);

static struct attribute *end_attrs[] = {
	&enable_attr.attr,
	&nd_setpoint_attr.attr,
	&mode_attr.attr,
	&nd_actual_attr.attr,
	&cell_temp_attr.attr,
	&status_attr.attr,
	&fault_clear_attr.attr,
	NULL,
};

static const struct attribute_group end_attr_group = {
	.attrs = end_attrs,
};

/* ------------------------------------------------------------------ */
/* Module init / exit                                                   */
/* ------------------------------------------------------------------ */

static int __init cinema_end_init(void)
{
	int ret;

	end_kobj = kobject_create_and_add("cinema_end", kernel_kobj);
	if (!end_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(end_kobj, &end_attr_group);
	if (ret) {
		kobject_put(end_kobj);
		return ret;
	}

	pr_info("cinema_end: eND interface ready\n");
	pr_info("cinema_end:   /sys/kernel/cinema_end/{enable,nd_setpoint,mode,nd_actual,cell_temp,status,fault_clear}\n");
	pr_info("cinema_end:   Rev A working band: 2000–4000 mb (2–4 stops), fault threshold: 55°C\n");
	return 0;
}

static void __exit cinema_end_exit(void)
{
	sysfs_remove_group(end_kobj, &end_attr_group);
	kobject_put(end_kobj);
	pr_info("cinema_end: unloaded\n");
}

module_init(cinema_end_init);
module_exit(cinema_end_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Electronic neutral density (eND) control interface for Xiaomi 14 Ultra");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
MODULE_SOFTDEP("pre: cinema_mode");
