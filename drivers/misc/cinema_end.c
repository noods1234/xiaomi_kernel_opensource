// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_end.c — Electronic neutral density (eND) control interface
 *
 * For Xiaomi 14 Ultra (aurora) / SM8650 (Pineapple).
 *
 * Hardware: LC-Tec PolarView eND(NBf2.0) dual-cell guest-host cartridge,
 * driven by an STM32U5 + AD5696R + TPS65131 sidecar board connected via
 * USB-CDC (VID 0x0483 / PID 0x5740).
 *
 * Architecture
 * ------------
 * This driver owns the kernel-side state machine and sysfs interface.
 * The MCU bridge daemon communicates in two ways:
 *
 *   1. Opens /dev/cinema_end (exclusive).  Holding the fd signals MCU
 *      presence.  Closing it (including on daemon crash via OS cleanup)
 *      atomically transitions the kernel to offline — no daemon cooperation
 *      required.  This is more reliable than a write-only sysfs node that
 *      stays set if the daemon crashes.
 *
 *   2. Reads/writes sysfs nodes for telemetry and setpoints.
 *
 * State machine
 * -------------
 *   [offline] ←→ open()/close() on /dev/cinema_end ←→ [standby]
 *   [standby]  enable=1 → [active]
 *   [active]   enable=0 → [standby]
 *   [active]   cell_temp > FAULT_MC or /dev/close → [fault]
 *   [fault]    disable + fault_clear=1 → [standby]
 *
 *   Temperature faults are NOT auto-cleared on daemon reconnect.
 *   The MCU must confirm cell temperature is below threshold before the
 *   daemon writes fault_clear=1; kernel cannot know this from a connect
 *   event alone.
 *
 * Temperature thresholds
 * ----------------------
 *   END_TEMP_WARN_MC  (55 000 m°C / 55°C): kobject_uevent KOBJ_CHANGE
 *     with CINEMA_EVENT=TEMP_WARN.  Non-fatal; operator is notified.
 *   END_TEMP_FAULT_MC (65 000 m°C / 65°C): enter fault state, release
 *     cinema_mode coordinator, sysfs_notify status.
 *
 *   The LC-Tec PolarView cartridge is rated to 70°C continuous use.
 *   55°C is a soft warning; 65°C is the hard-stop with 5°C margin.
 *
 * Sysfs nodes (under /sys/kernel/cinema_end/):
 *
 *   enable       rw  0=standby, 1=active.  -ENODEV if offline or faulted.
 *                    Writing 1 activates cinema_mode coordinator.
 *                    Requires CAP_SYS_ADMIN.
 *
 *   nd_setpoint  rw  Target ND in millibels [0, 7000].
 *                    0 means "park/clear" — daemon must apply park sequence.
 *                    Requires CAP_SYS_ADMIN.  sysfs_notify on change.
 *
 *   mode         rw  0=manual, 1=exposure_hold, 2=dof_hold.
 *                    Requires CAP_SYS_ADMIN.
 *
 *   nd_actual    rw  MCU-reported ND in millibels [0, 7000].
 *                    Written by daemon.  Requires CAP_SYS_ADMIN.
 *
 *   cell_temp    rw  Cell temperature in m°C [-40000, 125000].
 *                    Written by daemon.  Triggers TEMP_WARN at 55°C,
 *                    fault at 65°C.  Requires CAP_SYS_ADMIN.
 *
 *   status       ro  "offline" | "standby" | "active" | "fault"
 *
 *   fault_clear  wo  Write "1" after MCU confirms recovery.  -ENODEV if
 *                    offline; -EBUSY if active.  Requires CAP_SYS_ADMIN.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#include "cinema.h"
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define END_ND_MAX_MB		7000

#define END_MODE_MANUAL		0
#define END_MODE_EXPOSURE_HOLD	1
#define END_MODE_DOF_HOLD	2
#define END_MODE_MAX		END_MODE_DOF_HOLD

/*
 * Two-tier temperature protection.
 * WARN: kobject_uevent notifies the camera app; recording continues.
 * FAULT: hard stop, cinema_mode released, sysfs status → "fault".
 */
#define END_TEMP_WARN_MC	55000	/* 55°C — soft warning */
#define END_TEMP_FAULT_MC	65000	/* 65°C — hard fault, 5°C below 70°C rating */

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static bool end_mcu_online;	/* /dev/cinema_end is held open by daemon */
static bool end_active;
static bool end_fault;
static int  end_nd_setpoint;
static int  end_mode;
static int  end_nd_actual;
static int  end_cell_temp;

static struct kobject    *end_kobj;
static DEFINE_MUTEX(end_lock);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * end_enter_fault - transition to fault state.
 * Sets end_fault, clears end_active, releases cinema_mode, notifies sysfs.
 * Must be called with end_lock held.
 * Lock ordering: end_lock → cinema_lock; never reversed.
 */
static void end_enter_fault(const char *reason)
{
	if (end_fault)
		return;

	end_fault  = true;
	end_active = false;
	cinema_mode_set_active(false);
	pr_warn("cinema_end: fault — %s\n", reason);
	sysfs_notify(end_kobj, NULL, "status");
}

static const char *end_status_str(void)
{
	if (!end_mcu_online)
		return "offline";
	if (end_fault)
		return "fault";
	if (end_active)
		return "active";
	return "standby";
}

/* ------------------------------------------------------------------ */
/* /dev/cinema_end miscdev — MCU bridge daemon presence                 */
/* ------------------------------------------------------------------ */

/*
 * cinema_end_open - daemon connects; MCU is considered online.
 *
 * Exclusive: only one daemon may hold the device at a time.
 * The OS releases the fd on daemon exit/crash, triggering release().
 */
static int cinema_end_open(struct inode *inode, struct file *filp)
{
	mutex_lock(&end_lock);

	if (end_mcu_online) {
		mutex_unlock(&end_lock);
		return -EBUSY;		/* another daemon instance is running */
	}

	end_mcu_online = true;
	pr_info("cinema_end: MCU bridge daemon connected (pid %d)\n",
		task_pid_nr(current));
	sysfs_notify(end_kobj, NULL, "status");

	mutex_unlock(&end_lock);
	return 0;
}

/*
 * cinema_end_release - daemon disconnects or crashes.
 *
 * Temperature faults are NOT cleared on reconnect: the LC cell may still
 * be above threshold when the daemon restarts.  The daemon must write
 * fault_clear=1 only after MCU telemetry confirms recovery.
 */
static int cinema_end_release(struct inode *inode, struct file *filp)
{
	mutex_lock(&end_lock);

	end_mcu_online = false;

	if (end_active) {
		/* end_enter_fault() logs the disconnect reason as pr_warn */
		end_enter_fault("MCU bridge daemon disconnected during recording");
	} else {
		pr_info("cinema_end: MCU bridge daemon disconnected\n");
	}
	sysfs_notify(end_kobj, NULL, "status");

	mutex_unlock(&end_lock);
	return 0;
}

static const struct file_operations cinema_end_fops = {
	.owner   = THIS_MODULE,
	.open    = cinema_end_open,
	.release = cinema_end_release,
	.llseek  = no_llseek,
};

static struct miscdevice end_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "cinema_end",
	.fops  = &cinema_end_fops,
	.mode  = 0600,
};

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

	if (on && (!end_mcu_online || end_fault)) {
		ret = -ENODEV;
		goto out;
	}

	/*
	 * Activate the performance coordinator BEFORE setting end_active so
	 * that concurrent status_show() callers never see "active" with the
	 * coordinator not yet running.  On deactivation, cinema_deactivate()
	 * is void (always succeeds), so the order is safe in both directions.
	 */
	ret = cinema_mode_set_active(on);
	if (ret)
		goto out;

	end_active = on;
	if (!on)
		end_fault = false;

	pr_debug("cinema_end: eND %s\n", on ? "active" : "standby");
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

	/*
	 * Wake the daemon.  A setpoint of 0 means "park/clear" — the daemon
	 * applies the MCU park sequence; the kernel does not interpret it
	 * differently at the sysfs level.
	 */
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

	return count;
}

/* ------------------------------------------------------------------ */
/* nd_actual                                                            */
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
/* cell_temp                                                            */
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

	if (val < -40000 || val > 125000)
		return -ERANGE;

	mutex_lock(&end_lock);

	end_cell_temp = val;

	if (val > END_TEMP_FAULT_MC && end_active) {
		end_enter_fault("cell temperature exceeded 65°C hard limit");
	} else if (val > END_TEMP_WARN_MC && end_active) {
		/*
		 * Soft warning: notify userspace via uevent so the camera app
		 * can alert the operator.  Recording continues.
		 * sysfs_notify is not used here — status string does not change.
		 */
		char *envp[] = { "CINEMA_EVENT=TEMP_WARN", NULL };

		mutex_unlock(&end_lock);
		kobject_uevent_env(end_kobj, KOBJ_CHANGE, envp);
		pr_warn_ratelimited("cinema_end: cell temp warning: %d m°C "
				    "(warn=%d fault=%d)\n",
				    val, END_TEMP_WARN_MC, END_TEMP_FAULT_MC);
		return count;
	}

	mutex_unlock(&end_lock);
	return count;
}

/* ------------------------------------------------------------------ */
/* status                                                               */
/* ------------------------------------------------------------------ */

static ssize_t status_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	const char *state;

	mutex_lock(&end_lock);
	state = end_status_str();
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%s\n", state);
}

/* ------------------------------------------------------------------ */
/* fault_clear                                                          */
/* ------------------------------------------------------------------ */

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
		return count;

	mutex_lock(&end_lock);

	if (!end_mcu_online) {
		mutex_unlock(&end_lock);
		return -ENODEV;
	}

	if (end_active) {
		mutex_unlock(&end_lock);
		return -EBUSY;
	}

	if (end_fault) {
		end_fault = false;
		pr_info("cinema_end: fault cleared by daemon\n");
		sysfs_notify(end_kobj, NULL, "status");
	}

	mutex_unlock(&end_lock);
	return count;
}

/* ------------------------------------------------------------------ */
/* Attribute wiring                                                     */
/* ------------------------------------------------------------------ */

static struct kobj_attribute enable_attr =
	__ATTR(enable,       0600, enable_show,      enable_store);
static struct kobj_attribute nd_setpoint_attr =
	__ATTR(nd_setpoint,  0600, nd_setpoint_show, nd_setpoint_store);
static struct kobj_attribute mode_attr =
	__ATTR(mode,         0600, mode_show,        mode_store);
static struct kobj_attribute nd_actual_attr =
	__ATTR(nd_actual,    0600, nd_actual_show,   nd_actual_store);
static struct kobj_attribute cell_temp_attr =
	__ATTR(cell_temp,    0600, cell_temp_show,   cell_temp_store);
static struct kobj_attribute status_attr =
	__ATTR(status,       0444, status_show,      NULL);
static struct kobj_attribute fault_clear_attr =
	__ATTR(fault_clear,  0200, NULL,             fault_clear_store);

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

	/*
	 * Create the kobject and sysfs group BEFORE registering the miscdev.
	 *
	 * If misc_register() ran first, a daemon could open /dev/cinema_end
	 * before end_kobj is initialised.  cinema_end_open() calls
	 * sysfs_notify(end_kobj, ...) — sysfs_notify(NULL, ...) is a NULL
	 * dereference and panics the kernel.
	 */
	end_kobj = kobject_create_and_add("cinema_end", kernel_kobj);
	if (!end_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(end_kobj, &end_attr_group);
	if (ret)
		goto err_kobj;

	ret = misc_register(&end_miscdev);
	if (ret) {
		pr_err("cinema_end: failed to register miscdev (%d)\n", ret);
		goto err_sysfs;
	}

	pr_info("cinema_end: ready — open /dev/cinema_end to register MCU bridge daemon\n");
	pr_info("cinema_end:   /sys/kernel/cinema_end/{enable,nd_setpoint,mode,nd_actual,cell_temp,status,fault_clear}\n");
	pr_info("cinema_end:   temp thresholds: warn=%d m°C fault=%d m°C\n",
		END_TEMP_WARN_MC, END_TEMP_FAULT_MC);
	return 0;

err_sysfs:
	sysfs_remove_group(end_kobj, &end_attr_group);
err_kobj:
	kobject_put(end_kobj);
	return ret;
}

static void __exit cinema_end_exit(void)
{
	mutex_lock(&end_lock);
	if (end_active) {
		cinema_mode_set_active(false);
		end_active = false;
	}
	mutex_unlock(&end_lock);

	/*
	 * Deregister the miscdev BEFORE releasing the kobject.
	 *
	 * If kobject_put() ran first and the module is force-unloaded with the
	 * daemon fd still open, a subsequent cinema_end_release() would call
	 * sysfs_notify(end_kobj, ...) on freed memory.  Deregistering first
	 * prevents new opens; the module refcount (via THIS_MODULE in fops)
	 * ensures the module body stays mapped until all existing fds are closed,
	 * at which point release() has already been called before we get here.
	 */
	misc_deregister(&end_miscdev);
	sysfs_remove_group(end_kobj, &end_attr_group);
	kobject_put(end_kobj);
	pr_info("cinema_end: unloaded\n");
}

module_init(cinema_end_init);
module_exit(cinema_end_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Electronic neutral density (eND) control interface for Xiaomi 14 Ultra");
MODULE_AUTHOR("Xiaomi Cinema Kernel Project");
MODULE_SOFTDEP("pre: cinema_mode");
