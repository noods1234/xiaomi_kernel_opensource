// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thermal Message Interface — MI_THERMAL_MULTI_CHARGE
 *
 * Implements the sysfs interface referenced by CONFIG_MI_THERMAL_MULTI_CHARGE.
 * Registers a device named "thermal_message" under the existing "thermal"
 * class, producing /sys/class/thermal/thermal_message/<node>.
 *
 * Confirmed production nodes (source: aurora stock firmware init.target.rc
 * dump, 2026-03-20 — verified on ishtar/SM8550; aurora/SM8650 interface is
 * compatible per MI_THERMAL_MULTI_CHARGE ABI):
 *   sconfig            - scene configuration written by thermal daemon
 *   temp_state         - thermal mitigation level written by thermal daemon
 *   flash_state        - camera flash thermal state
 *   charger_temp       - charger temperature hint
 *   balance_mode       - balance/performance mode hint
 *   torch_real_level   - torch brightness level
 *   board_sensor_temp_comp - board sensor temperature compensation
 *   cpu_nolimit_temp   - CPU no-limit temperature threshold
 *
 * Value ranges: The kernel does not enforce semantic ranges.  The thermal
 * daemon owns all value semantics; kernel-side range checks would silently
 * reject valid daemon writes if the encoding ever changed.
 */

#define pr_fmt(fmt) "thermal_message: " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>

struct thermal_message_dev {
	struct device		*dev;
	int			sconfig;
	int			temp_state;
	int			flash_state;
	int			charger_temp;
	int			balance_mode;
	int			torch_real_level;
	int			board_sensor_temp_comp;
	int			cpu_nolimit_temp;
	struct mutex		lock;
};

static struct thermal_message_dev *tm_dev;

/* --- sconfig ------------------------------------------------------------ */

static ssize_t sconfig_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->sconfig;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t sconfig_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->sconfig = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(sconfig);

/* --- temp_state --------------------------------------------------------- */

static ssize_t temp_state_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->temp_state;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t temp_state_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->temp_state = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(temp_state);

/* --- flash_state -------------------------------------------------------- */

static ssize_t flash_state_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->flash_state;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t flash_state_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->flash_state = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(flash_state);

/* --- charger_temp ------------------------------------------------------- */

static ssize_t charger_temp_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->charger_temp;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t charger_temp_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->charger_temp = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(charger_temp);

/* --- balance_mode ------------------------------------------------------- */

static ssize_t balance_mode_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->balance_mode;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t balance_mode_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->balance_mode = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(balance_mode);

/* --- torch_real_level --------------------------------------------------- */

static ssize_t torch_real_level_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->torch_real_level;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t torch_real_level_store(struct device *dev, struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->torch_real_level = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(torch_real_level);

/* --- board_sensor_temp_comp --------------------------------------------- */

static ssize_t board_sensor_temp_comp_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->board_sensor_temp_comp;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t board_sensor_temp_comp_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->board_sensor_temp_comp = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(board_sensor_temp_comp);

/* --- cpu_nolimit_temp --------------------------------------------------- */

static ssize_t cpu_nolimit_temp_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int val;

	mutex_lock(&tm_dev->lock);
	val = tm_dev->cpu_nolimit_temp;
	mutex_unlock(&tm_dev->lock);
	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t cpu_nolimit_temp_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	int val, ret;

	ret = kstrtoint(strstrip((char *)buf), 0, &val);
	if (ret)
		return ret;
	mutex_lock(&tm_dev->lock);
	tm_dev->cpu_nolimit_temp = val;
	mutex_unlock(&tm_dev->lock);
	return count;
}
static DEVICE_ATTR_RW(cpu_nolimit_temp);

/* --- attribute group ---------------------------------------------------- */

static struct attribute *thermal_message_attrs[] = {
	&dev_attr_sconfig.attr,
	&dev_attr_temp_state.attr,
	&dev_attr_flash_state.attr,
	&dev_attr_charger_temp.attr,
	&dev_attr_balance_mode.attr,
	&dev_attr_torch_real_level.attr,
	&dev_attr_board_sensor_temp_comp.attr,
	&dev_attr_cpu_nolimit_temp.attr,
	NULL,
};
ATTRIBUTE_GROUPS(thermal_message);

/* --- module init/exit --------------------------------------------------- */

static int __init thermal_message_init(void)
{
	tm_dev = kzalloc(sizeof(*tm_dev), GFP_KERNEL);
	if (!tm_dev)
		return -ENOMEM;

	mutex_init(&tm_dev->lock);

	/*
	 * Register under the existing "thermal" class so the device appears
	 * at /sys/class/thermal/thermal_message/ — matching the production
	 * path used by the Xiaomi 14 Ultra (aurora) thermal daemon.
	 * Source: aurora firmware dump init.target.rc, 2026-03-20.
	 */
	tm_dev->dev = device_create_with_groups(&thermal_class, NULL,
						MKDEV(0, 0), NULL,
						thermal_message_groups,
						"thermal_message");
	if (IS_ERR(tm_dev->dev)) {
		pr_err("failed to create thermal_message device\n");
		mutex_destroy(&tm_dev->lock);
		kfree(tm_dev);
		return PTR_ERR(tm_dev->dev);
	}

	pr_info("thermal message interface registered\n");
	return 0;
}

static void __exit thermal_message_exit(void)
{
	if (!tm_dev)
		return;
	device_destroy(&thermal_class, MKDEV(0, 0));
	mutex_destroy(&tm_dev->lock);
	kfree(tm_dev);
	tm_dev = NULL;
}

module_init(thermal_message_init);
module_exit(thermal_message_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Xiaomi thermal message interface");
