// SPDX-License-Identifier: GPL-2.0-only
/*
 * xiaomi_usb_dock.c - Xiaomi USB-C Dock / Daughter Board Driver
 *
 * Handles detection, enumeration and power management of the Xiaomi
 * USB-C dock daughter board. The dock attaches to the phone/tablet via
 * a single USB Type-C cable and exposes:
 *
 *  • USB 3.1 Gen 2 hub (GL3523 or compatible)
 *  • DisplayPort 1.4 Alt Mode (VESA DP Alt Mode SVID 0xFF01)
 *  • HDMI 2.0 via on-dock DP-to-HDMI bridge (AG9311 or compatible)
 *  • Gigabit Ethernet (AX88179A or compatible, USB3->GbE)
 *  • USB-PD pass-through charging (up to 65 W)
 *  • UHS-I SD / microSD card reader (RTS5328 or compatible)
 *  • 3.5 mm analog audio jack (via phone USB-C to audio adapter path)
 *
 * Compatible Xiaomi SoC platforms:
 *   Qualcomm: SM8250/SM8350/SM8450/SM8550/SM8750 (SM8xxx family)
 *   MediaTek: MT6983/MT6985 (Dimensity 9000/9200 family)
 *
 * Target devices (representative):
 *   Xiaomi 14 Ultra, Xiaomi 15 Ultra, Xiaomi Pad 6 Pro,
 *   Xiaomi Pad 6S Pro, Xiaomi Pad 7 Pro, MIX Fold 4
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/usb.h>
#include <linux/usb/typec.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/role.h>
#include <linux/extcon-provider.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/notifier.h>

#include <linux/xiaomi-dock/xiaomi_usb_dock.h>

/* ----------------------------------------------------------------
 * Module parameters
 * ---------------------------------------------------------------- */

static bool dock_dp_enable = true;
module_param(dock_dp_enable, bool, 0644);
MODULE_PARM_DESC(dock_dp_enable,
	"Enable DisplayPort Alt Mode output (default: Y)");

static int dock_pd_max_mw = XIAOMI_DOCK_PD_PASSTHRU_MAX_MW;
module_param(dock_pd_max_mw, int, 0644);
MODULE_PARM_DESC(dock_pd_max_mw,
	"Maximum PD pass-through power in mW (default: 65000)");

/* ----------------------------------------------------------------
 * extcon cable IDs reported by this driver
 * ---------------------------------------------------------------- */

static const unsigned int xiaomi_dock_extcon_cables[] = {
	EXTCON_USB_HOST,
	EXTCON_DISP_DP,
	EXTCON_NONE,
};

/* ----------------------------------------------------------------
 * Helper: state name string (for dev_dbg / sysfs)
 * ---------------------------------------------------------------- */

static const char *dock_state_name(enum xiaomi_dock_state s)
{
	switch (s) {
	case DOCK_STATE_DISCONNECTED: return "disconnected";
	case DOCK_STATE_DETECTING:    return "detecting";
	case DOCK_STATE_ENUMERATING:  return "enumerating";
	case DOCK_STATE_CONNECTED:    return "connected";
	case DOCK_STATE_SUSPEND:      return "suspended";
	case DOCK_STATE_ERROR:        return "error";
	default:                      return "unknown";
	}
}

/* ----------------------------------------------------------------
 * State transition
 * ---------------------------------------------------------------- */

static void dock_set_state(struct xiaomi_usb_dock *dock,
			   enum xiaomi_dock_state new_state)
{
	if (dock->state == new_state)
		return;

	dev_dbg(dock->dev, "dock state: %s -> %s\n",
		dock_state_name(dock->state),
		dock_state_name(new_state));

	dock->state = new_state;
}

/* ----------------------------------------------------------------
 * Power: enable / disable VBUS to dock
 * ---------------------------------------------------------------- */

static int dock_vbus_enable(struct xiaomi_usb_dock *dock)
{
	int ret;

	if (!dock->vbus_reg)
		return 0;

	ret = regulator_enable(dock->vbus_reg);
	if (ret)
		dev_err(dock->dev, "failed to enable VBUS regulator: %d\n", ret);
	else
		dev_dbg(dock->dev, "VBUS enabled\n");

	return ret;
}

static void dock_vbus_disable(struct xiaomi_usb_dock *dock)
{
	if (!dock->vbus_reg)
		return;

	regulator_disable(dock->vbus_reg);
	dev_dbg(dock->dev, "VBUS disabled\n");
}

/* ----------------------------------------------------------------
 * USB role: switch host / device
 * ---------------------------------------------------------------- */

static int dock_set_usb_host_mode(struct xiaomi_usb_dock *dock)
{
	int ret;

	if (!dock->role_sw)
		return 0;

	ret = usb_role_switch_set_role(dock->role_sw, USB_ROLE_HOST);
	if (ret)
		dev_err(dock->dev, "failed to set USB host role: %d\n", ret);
	else
		dev_dbg(dock->dev, "USB role -> HOST\n");

	return ret;
}

static int dock_set_usb_device_mode(struct xiaomi_usb_dock *dock)
{
	int ret;

	if (!dock->role_sw)
		return 0;

	ret = usb_role_switch_set_role(dock->role_sw, USB_ROLE_DEVICE);
	if (ret)
		dev_err(dock->dev, "failed to set USB device role: %d\n", ret);
	else
		dev_dbg(dock->dev, "USB role -> DEVICE\n");

	return ret;
}

/* ----------------------------------------------------------------
 * DisplayPort Alt Mode
 * ---------------------------------------------------------------- */

static int dock_dp_altmode_notify(struct typec_altmode *altmode,
				  unsigned long conf, void *data)
{
	struct xiaomi_usb_dock *dock = data;
	bool active = !!(conf & TYPEC_STATE_ACTIVE);

	dev_dbg(dock->dev, "DP Alt Mode %s\n", active ? "active" : "inactive");

	mutex_lock(&dock->lock);
	dock->dp_connected = active;
	if (active) {
		/*
		 * Determine lane count from pin assignment:
		 * Pin C/D => 2 lanes (USB 3.x + 2-lane DP)
		 * Pin E/F => 4 lanes (4-lane DP, no USB 3.x SuperSpeed)
		 */
		u8 pin_assign = (conf >> 8) & 0xFF;

		dock->dp_lanes =
			(pin_assign & (XIAOMI_DOCK_DP_PIN_E |
				       XIAOMI_DOCK_DP_PIN_F)) ? 4 : 2;

		dev_info(dock->dev,
			 "DP Alt Mode: %d lanes, pin 0x%02x\n",
			 dock->dp_lanes, pin_assign);

		extcon_set_state_sync(dock->extcon, EXTCON_DISP_DP, true);
	} else {
		dock->dp_lanes = 0;
		extcon_set_state_sync(dock->extcon, EXTCON_DISP_DP, false);
	}
	mutex_unlock(&dock->lock);

	return 0;
}

static const struct typec_altmode_ops dock_dp_altmode_ops = {
	.attention = dock_dp_altmode_notify,
};

static int dock_register_dp_altmode(struct xiaomi_usb_dock *dock)
{
	struct typec_altmode_desc desc = {
		.svid  = XIAOMI_DOCK_DP_SVID,
		.mode  = 1,
		.vdo   = XIAOMI_DOCK_DP_PIN_C | XIAOMI_DOCK_DP_PIN_D |
			 XIAOMI_DOCK_DP_PIN_E | XIAOMI_DOCK_DP_PIN_F,
		.roles = TYPEC_PORT_DFP,
	};

	dock->dp_altmode = typec_port_register_altmode(dock->typec_port, &desc);
	if (IS_ERR(dock->dp_altmode)) {
		int ret = PTR_ERR(dock->dp_altmode);

		dock->dp_altmode = NULL;
		dev_warn(dock->dev,
			 "DP Alt Mode registration failed: %d (continuing without DP)\n",
			 ret);
		dock->capabilities &= ~XIAOMI_DOCK_CAP_DP;
		return ret;
	}

	typec_altmode_set_ops(dock->dp_altmode, &dock_dp_altmode_ops);
	typec_altmode_set_drvdata(dock->dp_altmode, dock);

	dev_info(dock->dev, "DP Alt Mode registered (SVID 0x%04x)\n",
		 XIAOMI_DOCK_DP_SVID);
	return 0;
}

/* ----------------------------------------------------------------
 * Connection / disconnection handlers
 * ---------------------------------------------------------------- */

void xiaomi_dock_notify_connect(struct xiaomi_usb_dock *dock)
{
	int ret;

	mutex_lock(&dock->lock);

	if (dock->state != DOCK_STATE_DISCONNECTED) {
		dev_warn(dock->dev, "connect event in state %s — ignoring\n",
			 dock_state_name(dock->state));
		mutex_unlock(&dock->lock);
		return;
	}

	dock_set_state(dock, DOCK_STATE_DETECTING);
	mutex_unlock(&dock->lock);

	/* Enable VBUS so dock MCU powers up */
	ret = dock_vbus_enable(dock);
	if (ret) {
		dock_set_state(dock, DOCK_STATE_ERROR);
		return;
	}

	/* Switch phone USB controller to host mode */
	ret = dock_set_usb_host_mode(dock);
	if (ret) {
		dock_vbus_disable(dock);
		dock_set_state(dock, DOCK_STATE_ERROR);
		return;
	}

	/* Allow dock hub to enumerate (USB 2.0: 100 ms; USB 3.x: 200 ms) */
	msleep(200);

	mutex_lock(&dock->lock);
	dock_set_state(dock, DOCK_STATE_ENUMERATING);
	mutex_unlock(&dock->lock);

	/* extcon: signal USB host active */
	extcon_set_state_sync(dock->extcon, EXTCON_USB_HOST, true);

	/* Schedule deferred work to verify enumeration completed */
	schedule_delayed_work(&dock->state_work, msecs_to_jiffies(1000));
}
EXPORT_SYMBOL_GPL(xiaomi_dock_notify_connect);

void xiaomi_dock_notify_disconnect(struct xiaomi_usb_dock *dock)
{
	mutex_lock(&dock->lock);

	if (dock->state == DOCK_STATE_DISCONNECTED) {
		mutex_unlock(&dock->lock);
		return;
	}

	cancel_delayed_work(&dock->state_work);

	/* Tear down DP Alt Mode */
	if (dock->dp_connected) {
		extcon_set_state_sync(dock->extcon, EXTCON_DISP_DP, false);
		dock->dp_connected = false;
		dock->dp_lanes = 0;
	}

	/* Revert USB role to device (peripheral) mode */
	dock_set_usb_device_mode(dock);

	/* Kill VBUS */
	dock_vbus_disable(dock);

	/* Clear child device pointers */
	dock->hub_udev = NULL;
	dock->lan_udev = NULL;
	dock->sd_udev  = NULL;

	extcon_set_state_sync(dock->extcon, EXTCON_USB_HOST, false);

	dock_set_state(dock, DOCK_STATE_DISCONNECTED);
	mutex_unlock(&dock->lock);

	dev_info(dock->dev, "dock disconnected\n");
}
EXPORT_SYMBOL_GPL(xiaomi_dock_notify_disconnect);

/* ----------------------------------------------------------------
 * Deferred work: confirm enumeration
 * ---------------------------------------------------------------- */

static void dock_state_work_fn(struct work_struct *work)
{
	struct xiaomi_usb_dock *dock =
		container_of(work, struct xiaomi_usb_dock, state_work.work);

	mutex_lock(&dock->lock);

	if (dock->state != DOCK_STATE_ENUMERATING) {
		mutex_unlock(&dock->lock);
		return;
	}

	/*
	 * In a full implementation the driver would walk the USB bus tree
	 * here, find the dock hub by VID/PID and store the udev pointers.
	 * For now we optimistically assume enumeration succeeded.
	 */
	dock_set_state(dock, DOCK_STATE_CONNECTED);
	mutex_unlock(&dock->lock);

	dev_info(dock->dev, "dock fully connected (caps: 0x%08x)\n",
		 dock->capabilities);
}

/* ----------------------------------------------------------------
 * USB notifier: track dock hub attach / detach
 * ---------------------------------------------------------------- */

static int dock_usb_notifier_call(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	struct usb_device *udev = data;
	struct xiaomi_usb_dock *dock =
		container_of(nb, struct xiaomi_usb_dock,
			     /* nb embedded in future extension struct */
			     state_work.work); /* placeholder — real impl
						  embeds nb in the device
						  structure */

	(void)dock;
	(void)udev;
	(void)action;

	return NOTIFY_OK;
}

/* ----------------------------------------------------------------
 * sysfs attributes
 * ---------------------------------------------------------------- */

static ssize_t state_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct xiaomi_usb_dock *dock = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", dock_state_name(dock->state));
}
static DEVICE_ATTR_RO(state);

static ssize_t capabilities_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xiaomi_usb_dock *dock = dev_get_drvdata(dev);
	int len = 0;

	if (dock->capabilities & XIAOMI_DOCK_CAP_USB_HUB)
		len += sysfs_emit_at(buf, len, "usb_hub ");
	if (dock->capabilities & XIAOMI_DOCK_CAP_DP)
		len += sysfs_emit_at(buf, len, "displayport ");
	if (dock->capabilities & XIAOMI_DOCK_CAP_HDMI)
		len += sysfs_emit_at(buf, len, "hdmi ");
	if (dock->capabilities & XIAOMI_DOCK_CAP_ETHERNET)
		len += sysfs_emit_at(buf, len, "ethernet ");
	if (dock->capabilities & XIAOMI_DOCK_CAP_PD_PASSTHRU)
		len += sysfs_emit_at(buf, len, "pd_passthru ");
	if (dock->capabilities & XIAOMI_DOCK_CAP_SD_READER)
		len += sysfs_emit_at(buf, len, "sd_reader ");
	if (dock->capabilities & XIAOMI_DOCK_CAP_AUDIO)
		len += sysfs_emit_at(buf, len, "audio ");

	if (len > 0)
		buf[len - 1] = '\n';

	return len;
}
static DEVICE_ATTR_RO(capabilities);

static ssize_t dp_lanes_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct xiaomi_usb_dock *dock = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", dock->dp_lanes);
}
static DEVICE_ATTR_RO(dp_lanes);

static ssize_t pd_contract_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct xiaomi_usb_dock *dock = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%dmV / %dmA\n",
			  dock->pd_voltage_mv, dock->pd_current_ma);
}
static DEVICE_ATTR_RO(pd_contract);

static struct attribute *dock_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_capabilities.attr,
	&dev_attr_dp_lanes.attr,
	&dev_attr_pd_contract.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dock);

/* ----------------------------------------------------------------
 * Platform probe / remove
 * ---------------------------------------------------------------- */

int xiaomi_dock_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xiaomi_usb_dock *dock;
	struct typec_capability typec_cap = {};
	int ret;

	dock = devm_kzalloc(dev, sizeof(*dock), GFP_KERNEL);
	if (!dock)
		return -ENOMEM;

	dock->dev = dev;
	mutex_init(&dock->lock);
	dock->state = DOCK_STATE_DISCONNECTED;
	INIT_DELAYED_WORK(&dock->state_work, dock_state_work_fn);

	/* ---- Read capabilities from device tree ---- */
	dock->capabilities = 0;

	if (of_property_read_bool(dev->of_node, "xiaomi,dock-has-usb-hub"))
		dock->capabilities |= XIAOMI_DOCK_CAP_USB_HUB;
	if (dock_dp_enable &&
	    of_property_read_bool(dev->of_node, "xiaomi,dock-has-dp"))
		dock->capabilities |= XIAOMI_DOCK_CAP_DP;
	if (of_property_read_bool(dev->of_node, "xiaomi,dock-has-hdmi"))
		dock->capabilities |= XIAOMI_DOCK_CAP_HDMI;
	if (of_property_read_bool(dev->of_node, "xiaomi,dock-has-ethernet"))
		dock->capabilities |= XIAOMI_DOCK_CAP_ETHERNET;
	if (of_property_read_bool(dev->of_node, "xiaomi,dock-has-pd-passthru"))
		dock->capabilities |= XIAOMI_DOCK_CAP_PD_PASSTHRU;
	if (of_property_read_bool(dev->of_node, "xiaomi,dock-has-sd-reader"))
		dock->capabilities |= XIAOMI_DOCK_CAP_SD_READER;
	if (of_property_read_bool(dev->of_node, "xiaomi,dock-has-audio"))
		dock->capabilities |= XIAOMI_DOCK_CAP_AUDIO;

	/* ---- VBUS regulator (optional: present when phone controls VBUS) ---- */
	dock->vbus_reg = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(dock->vbus_reg)) {
		if (PTR_ERR(dock->vbus_reg) != -ENODEV) {
			dev_err(dev, "failed to get VBUS regulator: %ld\n",
				PTR_ERR(dock->vbus_reg));
			return PTR_ERR(dock->vbus_reg);
		}
		dock->vbus_reg = NULL;
	}

	/* ---- USB role switch ---- */
	dock->role_sw = usb_role_switch_get(dev);
	if (IS_ERR(dock->role_sw)) {
		ret = PTR_ERR(dock->role_sw);
		if (ret != -ENODEV) {
			dev_err(dev, "failed to get USB role switch: %d\n", ret);
			return ret;
		}
		dock->role_sw = NULL;
	}

	/* ---- extcon ---- */
	dock->extcon = devm_extcon_dev_allocate(dev, xiaomi_dock_extcon_cables);
	if (IS_ERR(dock->extcon)) {
		dev_err(dev, "failed to allocate extcon device: %ld\n",
			PTR_ERR(dock->extcon));
		ret = PTR_ERR(dock->extcon);
		goto err_role_sw;
	}

	ret = devm_extcon_dev_register(dev, dock->extcon);
	if (ret) {
		dev_err(dev, "failed to register extcon device: %d\n", ret);
		goto err_role_sw;
	}

	/* ---- USB Type-C port ---- */
	typec_cap.type        = TYPEC_PORT_DRP;
	typec_cap.data        = TYPEC_PORT_DRD;
	typec_cap.revision    = USB_TYPEC_REV_1_3;
	typec_cap.pd_revision = 0x0300; /* USB PD 3.0 */
	typec_cap.prefer_role = TYPEC_SINK;   /* default: phone charges */
	typec_cap.driver_data = dock;

	dock->typec_port = typec_register_port(dev, &typec_cap);
	if (IS_ERR(dock->typec_port)) {
		ret = PTR_ERR(dock->typec_port);
		dev_err(dev, "failed to register USB Type-C port: %d\n", ret);
		goto err_role_sw;
	}

	/* ---- Register DP Alt Mode (if dock supports it) ---- */
	if (dock->capabilities & XIAOMI_DOCK_CAP_DP)
		dock_register_dp_altmode(dock);   /* non-fatal on failure */

	/* ---- sysfs ---- */
	ret = devm_device_add_groups(dev, dock_groups);
	if (ret) {
		dev_err(dev, "failed to create sysfs attributes: %d\n", ret);
		goto err_typec;
	}

	platform_set_drvdata(pdev, dock);

	dev_info(dev, "Xiaomi USB-C Dock driver loaded (caps: 0x%08x)\n",
		 dock->capabilities);
	return 0;

err_typec:
	if (dock->dp_altmode)
		typec_unregister_altmode(dock->dp_altmode);
	typec_unregister_port(dock->typec_port);
err_role_sw:
	if (dock->role_sw)
		usb_role_switch_put(dock->role_sw);
	return ret;
}
EXPORT_SYMBOL_GPL(xiaomi_dock_probe);

void xiaomi_dock_remove(struct platform_device *pdev)
{
	struct xiaomi_usb_dock *dock = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&dock->state_work);

	/* Ensure dock is fully disconnected */
	if (dock->state != DOCK_STATE_DISCONNECTED)
		xiaomi_dock_notify_disconnect(dock);

	if (dock->dp_altmode)
		typec_unregister_altmode(dock->dp_altmode);

	typec_unregister_port(dock->typec_port);

	if (dock->role_sw)
		usb_role_switch_put(dock->role_sw);
}
EXPORT_SYMBOL_GPL(xiaomi_dock_remove);

/* ----------------------------------------------------------------
 * PM callbacks
 * ---------------------------------------------------------------- */

int xiaomi_dock_suspend(struct device *dev)
{
	struct xiaomi_usb_dock *dock = dev_get_drvdata(dev);

	mutex_lock(&dock->lock);
	if (dock->state == DOCK_STATE_CONNECTED)
		dock_set_state(dock, DOCK_STATE_SUSPEND);
	mutex_unlock(&dock->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(xiaomi_dock_suspend);

int xiaomi_dock_resume(struct device *dev)
{
	struct xiaomi_usb_dock *dock = dev_get_drvdata(dev);

	mutex_lock(&dock->lock);
	if (dock->state == DOCK_STATE_SUSPEND)
		dock_set_state(dock, DOCK_STATE_CONNECTED);
	mutex_unlock(&dock->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(xiaomi_dock_resume);

/* ----------------------------------------------------------------
 * Device Tree match table
 * ---------------------------------------------------------------- */

static const struct of_device_id xiaomi_dock_of_match[] = {
	{ .compatible = "xiaomi,usb-dock-basic",  },
	{ .compatible = "xiaomi,usb-dock-pro",    },
	{ .compatible = "xiaomi,usb-dock-ultra",  },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, xiaomi_dock_of_match);

/* ----------------------------------------------------------------
 * Platform driver registration
 * ---------------------------------------------------------------- */

static const struct dev_pm_ops xiaomi_dock_pm_ops = {
	.suspend = xiaomi_dock_suspend,
	.resume  = xiaomi_dock_resume,
};

static struct platform_driver xiaomi_dock_driver = {
	.probe  = xiaomi_dock_probe,
	.remove = xiaomi_dock_remove,
	.driver = {
		.name           = "xiaomi-usb-dock",
		.of_match_table = xiaomi_dock_of_match,
		.pm             = &xiaomi_dock_pm_ops,
	},
};

module_platform_driver(xiaomi_dock_driver);

MODULE_AUTHOR("Xiaomi USB Dock Team");
MODULE_DESCRIPTION("Xiaomi USB-C Dock / Daughter Board Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");
