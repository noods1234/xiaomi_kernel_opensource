// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_end.c — Electronic neutral density (eND) control interface
 *
 * For Xiaomi 14 Ultra (aurora) / SM8650 (Pineapple).
 *
 * NOTE: device codename "aurora" is tentative — verify against the
 * shipping device-tree before tagging a production build.
 * Hardware: LC-Tec PolarView eND(NBf2.0) dual-cell guest-host cartridge,
 * driven by an STM32U5 + AD5696R + TPS65131 sidecar board via USB/UART.
 *
 * This driver owns the kernel-side control and telemetry interface.
 * The actual LC drive waveform generation is performed by a privileged
 * userspace daemon (cinema_end_daemon) that bridges between this sysfs
 * interface and the MCU over USB-CDC or UART.  The split keeps platform
 * driver complexity low and lets the MCU firmware be updated in the field
 * without a kernel change.
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
 *                    coordinator (see TODO below).
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
 *                    Used by the cinema app and available for logging.
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   status       ro  Human-readable state string: "standby", "active",
 *                    or "fault".  Reflects end_active and end_fault flags.
 *
 * Integration with cinema_mode
 * ----------------------------
 * TODO (Rev A-1): When eND is enabled, the performance coordinator
 * (cinema_mode.c) should also be activated to pin CPU frequencies, block
 * deep idle, and prevent suspend.  Two options:
 *
 *   Option A (simple, for Rev A prototype):
 *     cinema_end enable_store() writes 1 to /sys/kernel/cinema_mode/enable
 *     via kernel_write / sysfs_notify path — acceptable for a prototype but
 *     fragile because it depends on the sysfs path being stable.
 *
 *   Option B (production path):
 *     Export cinema_mode_set_active(bool) from cinema_mode.c and call it
 *     directly.  Requires adding EXPORT_SYMBOL_GPL in cinema_mode.c and
 *     a forward declaration here.  This is the correct long-term design.
 *
 * The stub below leaves both paths unimplemented and documents the
 * pr_info message that would be the call site.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include <linux/capability.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

/**
 * struct cinema_end_hw_ops - hardware backend operations for eND control
 *
 * All function pointers may be NULL — the driver operates as a pure
 * userspace-facing stub (register tracking only) when no backend is set.
 * A real backend (SPI MCU, I2C actuator) sets these via cinema_end_set_ops().
 */
struct cinema_end_hw_ops {
	/** @enable: power on/off the eND cell hardware */
	int (*enable)(bool on);
	/** @set_nd: command a new ND setpoint in millibels */
	int (*set_nd)(int millibels);
	/** @get_nd: read back the achieved ND from hardware */
	int (*get_nd)(int *millibels);
	/** @get_temp: read cell temperature in millidegrees C */
	int (*get_temp)(int *millidegrees);
};

static const struct cinema_end_hw_ops *end_hw_ops;  /* NULL = stub mode */

/* Forward declaration — allows platform drivers in other translation
 * units to register a backend without a separate header (YAGNI). */
void cinema_end_set_ops(const struct cinema_end_hw_ops *ops);

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
/* Hardware backend registration                                        */
/* ------------------------------------------------------------------ */

/**
 * cinema_end_set_ops - register (or unregister) a hardware backend
 * @ops: pointer to ops table, or NULL to revert to stub mode
 *
 * Called by a platform driver (e.g. SPI MCU bridge) once its hardware
 * is ready.  Pass NULL to detach.  Safe to call at any time after
 * module_init completes.
 */
void cinema_end_set_ops(const struct cinema_end_hw_ops *ops)
{
	mutex_lock(&end_lock);
	end_hw_ops = ops;
	mutex_unlock(&end_lock);
}
EXPORT_SYMBOL_GPL(cinema_end_set_ops);

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
	int val;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	val = !!val;

	mutex_lock(&end_lock);

	if (val == end_active)
		goto out;

	end_active = val;
	if (!val)
		end_fault = false;	/* clear fault on explicit disable */

	if (end_hw_ops && end_hw_ops->enable) {
		int hw_ret = end_hw_ops->enable(val);

		if (hw_ret)
			pr_warn("cinema_end: hw enable(%d) failed: %d\n", val, hw_ret);
	}

	/*
	 * TODO (Rev A-1): propagate to cinema_mode performance coordinator.
	 * See "Integration with cinema_mode" in the file header for options.
	 *
	 * When val == 1, also activate cinema_mode (CPU pin, latency QoS,
	 * wakeup source).  When val == 0 and cinema_mode was activated by us,
	 * release it — but only if the operator hasn't independently enabled
	 * cinema_mode.  Track ownership with a flag.
	 */
	pr_info("cinema_end: eND hardware %s\n", val ? "active" : "standby");

out:
	mutex_unlock(&end_lock);
	return count;
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
	if (end_hw_ops && end_hw_ops->set_nd) {
		int hw_ret = end_hw_ops->set_nd(val);

		if (hw_ret)
			pr_warn("cinema_end: hw set_nd(%d) failed: %d\n", val, hw_ret);
	}
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
	 * TODO (Rev A-1): raise a fault if temperature exceeds the MCU
	 * alert threshold (55 000 m°C / 55°C).  The TMP117 hardware alert
	 * fires first, but a kernel-level check provides a second safety
	 * net and allows the cinema app to react without polling.
	 */
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
	pr_info("cinema_end:   /sys/kernel/cinema_end/{enable,nd_setpoint,mode,nd_actual,cell_temp,status}\n");
	pr_info("cinema_end:   Rev A working band: 2000–4000 mb (2–4 stops)\n");
	pr_info("cinema_end: hardware backend: %s\n", end_hw_ops ? "registered" : "stub (no hardware)");
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
