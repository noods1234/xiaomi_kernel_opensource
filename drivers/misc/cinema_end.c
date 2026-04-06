// SPDX-License-Identifier: GPL-2.0-only
/*
 * cinema_end.c — Electronic neutral density (eND) control interface
 *
 * For Xiaomi 14 Ultra (aurora) / SM8650 (Pineapple).
 *
 * Hardware: LC-Tec PolarView eND(NBf2.0) dual-cell guest-host cartridge,
 * driven by an STM32U5 + AD5696R + TPS65131 sidecar board.  The MCU
 * connects to the phone via USB-CDC (VID 0x0483 / PID 0x5740) when the
 * cinema optical module is attached.
 *
 * Architecture
 * ------------
 * This driver owns the kernel-side control and telemetry interface.
 * The actual LC drive waveform generation is performed by a privileged
 * userspace daemon (cinema_end_daemon) that:
 *
 *   1. Monitors the USB bus for the STM32U5 device.
 *   2. Writes "1" to mcu_online when the MCU enumerates, "0" when it
 *      disconnects.
 *   3. Reads nd_setpoint (via poll + read) for setpoint changes from
 *      the camera app.
 *   4. Writes nd_actual and cell_temp to report MCU telemetry.
 *   5. Writes "1" to fault_clear after a fault condition is resolved.
 *
 * Kernel responsibilities:
 *   - State machine: offline → standby → active ↔ fault
 *   - sysfs interface for camera app → MCU command path
 *   - sysfs telemetry sink for MCU daemon → kernel → userspace path
 *   - ND setpoint validation
 *   - Temperature fault detection at END_TEMP_FAULT_MC (55°C)
 *   - State interlock with cinema_mode performance coordinator
 *   - sysfs_notify on every state transition so poll(2) waiters wake
 *
 * MCU presence state machine
 * --------------------------
 *
 *   mcu_online=0 → [offline]  mcu_online=1 → [standby]
 *   [standby]  enable=1 → [active]
 *   [active]   enable=0 → [standby]
 *   [active]   temp > 55°C or mcu_online=0 → [fault]
 *   [fault]    disable + fault_clear=1 → [standby]  (if MCU still online)
 *   [fault]    mcu_online=0 while faulted → [offline]  (hardware gone)
 *
 * Sysfs nodes (under /sys/kernel/cinema_end/):
 *
 *   mcu_online   rw  0 = MCU offline/disconnected, 1 = MCU online.
 *                    Written by the daemon on USB connect/disconnect.
 *                    Transitioning to 0 while active enters fault state.
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   enable       rw  0 = standby, 1 = active.
 *                    Requires CAP_SYS_ADMIN to write.
 *                    Returns -ENODEV if MCU is offline.
 *                    Writing 1 also activates cinema_mode coordinator.
 *
 *   nd_setpoint  rw  Target ND in millibels [0, END_ND_MAX_MB = 7000].
 *                    Requires CAP_SYS_ADMIN to write.
 *                    sysfs_notify fired on change (daemon uses poll).
 *
 *   mode         rw  0=manual, 1=exposure_hold, 2=dof_hold.
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   nd_actual    rw  MCU-reported current ND in millibels [0, 7000].
 *                    Written by daemon from MCU telemetry.
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   cell_temp    rw  LC cell temperature in m°C [-40000, 125000].
 *                    Written by daemon from TMP117 telemetry.
 *                    Triggers fault if > END_TEMP_FAULT_MC (55 000 m°C).
 *                    Requires CAP_SYS_ADMIN to write.
 *
 *   status       ro  "offline" | "standby" | "active" | "fault"
 *
 *   fault_clear  wo  Write "1" after MCU recovery to clear fault.
 *                    -EBUSY if end_active; -ENODEV if MCU offline.
 *                    No-op if not faulted or if value is "0".
 *                    Requires CAP_SYS_ADMIN.
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

#define END_ND_MAX_MB		7000

#define END_MODE_MANUAL		0
#define END_MODE_EXPOSURE_HOLD	1
#define END_MODE_DOF_HOLD	2
#define END_MODE_MAX		END_MODE_DOF_HOLD

/*
 * Temperature fault threshold: 55 000 m°C (55°C).
 * The LC-Tec cartridge operating limit under active drive.  The TMP117
 * hardware alert fires first; this is the kernel-side second net.
 */
#define END_TEMP_FAULT_MC	55000

/* ------------------------------------------------------------------ */
/* Inter-module API (from cinema_mode.c)                                */
/* ------------------------------------------------------------------ */

int cinema_mode_set_active(bool on);

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static bool end_mcu_online;	/* MCU (STM32U5) connected and enumerated */
static bool end_active;		/* eND hardware enabled */
static bool end_fault;		/* MCU or hardware fault signalled */
static int  end_nd_setpoint;
static int  end_mode;
static int  end_nd_actual;
static int  end_cell_temp;

static struct kobject *end_kobj;
static DEFINE_MUTEX(end_lock);

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * end_enter_fault - transition to fault state under end_lock.
 *
 * Sets end_fault, clears end_active, releases cinema_mode coordinator,
 * notifies sysfs waiters on "status".
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

/*
 * end_status_str - derive status string from current state.
 * Priority: offline > fault > active > standby.
 * Must be called with end_lock held (or during init before registration).
 */
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
/* mcu_online                                                           */
/* ------------------------------------------------------------------ */

static ssize_t mcu_online_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	bool online;

	mutex_lock(&end_lock);
	online = end_mcu_online;
	mutex_unlock(&end_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", online ? 1 : 0);
}

static ssize_t mcu_online_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	bool online;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (kstrtobool(buf, &online))
		return -EINVAL;

	mutex_lock(&end_lock);

	if (online == end_mcu_online)
		goto out;

	if (!online) {
		/*
		 * MCU disconnected.  If recording is active, this is a
		 * hardware fault — the optical cell lost its drive source.
		 * If standby, simply go offline.
		 */
		if (end_active)
			end_enter_fault("MCU disconnected during active recording");
		else if (end_fault)
			end_fault = false;	/* fault already cleared; go offline */

		end_mcu_online = false;
	} else {
		end_mcu_online = true;
		/* Fault from a previous MCU disconnect is automatically cleared
		 * when the MCU reconnects and the daemon writes mcu_online=1.
		 * The operator must still write enable=0 then enable=1 to restart
		 * recording; fault_clear is not needed in this path.
		 */
		if (end_fault && !end_active)
			end_fault = false;
	}

	pr_info("cinema_end: MCU %s\n", end_mcu_online ? "online" : "offline");
	sysfs_notify(end_kobj, NULL, "status");
	sysfs_notify(end_kobj, NULL, "mcu_online");

out:
	mutex_unlock(&end_lock);
	return count;
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

	if (on) {
		if (!end_mcu_online) {
			ret = -ENODEV;
			goto out;
		}
		if (end_fault) {
			ret = -ENODEV;
			goto out;
		}
	}

	end_active = on;
	if (!on)
		end_fault = false;

	ret = cinema_mode_set_active(on);
	if (ret && on) {
		end_active = false;
		goto out;
	}

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
	if (val > END_TEMP_FAULT_MC && end_active)
		end_enter_fault("cell temperature exceeded 55°C limit");
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

static struct kobj_attribute mcu_online_attr =
	__ATTR(mcu_online,   0600, mcu_online_show,   mcu_online_store);
static struct kobj_attribute enable_attr =
	__ATTR(enable,       0600, enable_show,        enable_store);
static struct kobj_attribute nd_setpoint_attr =
	__ATTR(nd_setpoint,  0600, nd_setpoint_show,   nd_setpoint_store);
static struct kobj_attribute mode_attr =
	__ATTR(mode,         0600, mode_show,          mode_store);
static struct kobj_attribute nd_actual_attr =
	__ATTR(nd_actual,    0600, nd_actual_show,     nd_actual_store);
static struct kobj_attribute cell_temp_attr =
	__ATTR(cell_temp,    0600, cell_temp_show,     cell_temp_store);
static struct kobj_attribute status_attr =
	__ATTR(status,       0444, status_show,        NULL);
static struct kobj_attribute fault_clear_attr =
	__ATTR(fault_clear,  0200, NULL,               fault_clear_store);

static struct attribute *end_attrs[] = {
	&mcu_online_attr.attr,
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

	pr_info("cinema_end: interface ready (status: offline — awaiting MCU)\n");
	pr_info("cinema_end:   /sys/kernel/cinema_end/{mcu_online,enable,nd_setpoint,mode,nd_actual,cell_temp,status,fault_clear}\n");
	return 0;
}

static void __exit cinema_end_exit(void)
{
	mutex_lock(&end_lock);
	if (end_active) {
		cinema_mode_set_active(false);
		end_active = false;
	}
	mutex_unlock(&end_lock);

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
