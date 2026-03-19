/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xiaomi_usb_dock.h - Xiaomi USB-C Dock/Daughter Board Driver Header
 *
 * Supports USB-C dock peripherals for Xiaomi smartphones and tablets.
 * Compatible with devices using Qualcomm SM8250/SM8350/SM8450/SM8550/SM8750
 * and MediaTek MT6983/MT6985 SoCs.
 *
 * Dock hardware provides:
 *   - USB 3.1 Gen2 host hub (up to 4x USB-A + 1x USB-C pass-through)
 *   - DisplayPort 1.4 Alt Mode output (up to 4K@60Hz)
 *   - HDMI 2.0 output (via DP-to-HDMI bridge)
 *   - Gigabit Ethernet (via USB3-to-GbE)
 *   - USB Power Delivery pass-through (up to 65W PD input)
 *   - SD/microSD card reader (UHS-I)
 *   - 3.5mm audio jack
 */

#ifndef __XIAOMI_USB_DOCK_H__
#define __XIAOMI_USB_DOCK_H__

#include <linux/device.h>
#include <linux/types.h>
#include <linux/extcon.h>
#include <linux/usb/typec.h>
#include <linux/usb/role.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>

/* ----------------------------------------------------------------
 * Dock identification
 * ---------------------------------------------------------------- */

/* USB Vendor/Product IDs used by the dock's internal hub IC */
#define XIAOMI_DOCK_VID			0x2717  /* Xiaomi vendor ID */
#define XIAOMI_DOCK_PID_BASIC		0x5001  /* Basic dock (USB hub only) */
#define XIAOMI_DOCK_PID_PRO		0x5002  /* Pro dock (hub + DP + LAN) */
#define XIAOMI_DOCK_PID_ULTRA		0x5003  /* Ultra dock (full feature set) */

/* BCD device version encoding:  0xMMmm */
#define XIAOMI_DOCK_BCD_MIN		0x0100
#define XIAOMI_DOCK_BCD_MAX		0x01FF

/* ----------------------------------------------------------------
 * Dock capability flags
 * ---------------------------------------------------------------- */

#define XIAOMI_DOCK_CAP_USB_HUB		BIT(0)  /* USB 3.x hub present */
#define XIAOMI_DOCK_CAP_DP		BIT(1)  /* DisplayPort Alt Mode */
#define XIAOMI_DOCK_CAP_HDMI		BIT(2)  /* HDMI output (DP bridge) */
#define XIAOMI_DOCK_CAP_ETHERNET	BIT(3)  /* Gigabit Ethernet */
#define XIAOMI_DOCK_CAP_PD_PASSTHRU	BIT(4)  /* USB-PD pass-through charging */
#define XIAOMI_DOCK_CAP_SD_READER	BIT(5)  /* SD/microSD card reader */
#define XIAOMI_DOCK_CAP_AUDIO		BIT(6)  /* 3.5mm audio jack */
#define XIAOMI_DOCK_CAP_USB_C_PD	BIT(7)  /* USB-C PD downstream port */

/* ----------------------------------------------------------------
 * Power Delivery configuration
 * ---------------------------------------------------------------- */

/* Maximum power the dock requests for its own circuitry (mW) */
#define XIAOMI_DOCK_SELF_POWER_MW	5000

/* Maximum PD pass-through charging power (mW) */
#define XIAOMI_DOCK_PD_PASSTHRU_MAX_MW	65000

/* Dock PD contract voltages supported (mV) */
#define XIAOMI_DOCK_PD_VOLT_5V		5000
#define XIAOMI_DOCK_PD_VOLT_9V		9000
#define XIAOMI_DOCK_PD_VOLT_12V		12000
#define XIAOMI_DOCK_PD_VOLT_15V		15000
#define XIAOMI_DOCK_PD_VOLT_20V		20000

/* ----------------------------------------------------------------
 * DisplayPort Alt Mode
 * ---------------------------------------------------------------- */

/* DP Alt Mode SVID (Standard-Defined by USB-IF / VESA) */
#define XIAOMI_DOCK_DP_SVID		0xFF01

/* DP pin assignments supported (from USB-C spec Table 6-30) */
#define XIAOMI_DOCK_DP_PIN_C		BIT(2)  /* DP 2-lane + USB 3.x */
#define XIAOMI_DOCK_DP_PIN_D		BIT(3)  /* DP 2-lane + USB 3.x (flipped) */
#define XIAOMI_DOCK_DP_PIN_E		BIT(4)  /* DP 4-lane */
#define XIAOMI_DOCK_DP_PIN_F		BIT(5)  /* DP 4-lane (flipped) */

/* ----------------------------------------------------------------
 * Dock state machine
 * ---------------------------------------------------------------- */

/**
 * enum xiaomi_dock_state - Dock attachment state
 * @DOCK_STATE_DISCONNECTED: No dock attached
 * @DOCK_STATE_DETECTING:    USB-C plug detected, running discovery
 * @DOCK_STATE_ENUMERATING:  USB device enumeration in progress
 * @DOCK_STATE_CONNECTED:    Dock fully operational
 * @DOCK_STATE_SUSPEND:      Dock suspended (system sleep)
 * @DOCK_STATE_ERROR:        Enumeration or power fault
 */
enum xiaomi_dock_state {
	DOCK_STATE_DISCONNECTED = 0,
	DOCK_STATE_DETECTING,
	DOCK_STATE_ENUMERATING,
	DOCK_STATE_CONNECTED,
	DOCK_STATE_SUSPEND,
	DOCK_STATE_ERROR,
};

/**
 * enum xiaomi_dock_type - Dock hardware variant
 * @DOCK_TYPE_UNKNOWN:  Not yet identified
 * @DOCK_TYPE_BASIC:    USB hub only
 * @DOCK_TYPE_PRO:      Hub + DP + LAN
 * @DOCK_TYPE_ULTRA:    Full feature set
 */
enum xiaomi_dock_type {
	DOCK_TYPE_UNKNOWN = 0,
	DOCK_TYPE_BASIC,
	DOCK_TYPE_PRO,
	DOCK_TYPE_ULTRA,
};

/* ----------------------------------------------------------------
 * Main driver structure
 * ---------------------------------------------------------------- */

/**
 * struct xiaomi_usb_dock - Per-device driver state
 * @dev:            Underlying kernel device
 * @lock:           Protects concurrent state transitions
 * @state:          Current dock state machine position
 * @type:           Identified dock hardware variant
 * @capabilities:   Bitmask of XIAOMI_DOCK_CAP_* flags
 *
 * @typec_port:     USB Type-C port handle (from typec_register_port)
 * @role_sw:        USB role switch (host / device)
 * @partner:        Connected USB-C partner descriptor
 *
 * @dp_altmode:     DisplayPort alternate mode handle
 * @dp_connected:   True when DP Alt Mode is active
 * @dp_lanes:       Active DP lane count (2 or 4)
 *
 * @vbus_reg:       Regulator supplying VBUS to dock (if phone-controlled)
 * @usb_psy:        USB power supply for PD negotiation
 * @pd_voltage_mv:  Currently contracted PD voltage
 * @pd_current_ma:  Currently contracted PD current
 *
 * @state_work:     Deferred work for state transitions
 * @extcon:         External connector (for legacy OTG detection)
 *
 * @hub_udev:       USB device handle for dock's root hub
 * @lan_udev:       USB device handle for GbE adapter
 * @sd_udev:        USB device handle for SD card reader
 */
struct xiaomi_usb_dock {
	struct device		*dev;
	struct mutex		lock;
	enum xiaomi_dock_state	state;
	enum xiaomi_dock_type	type;
	u32			capabilities;

	/* USB Type-C framework */
	struct typec_port	*typec_port;
	struct usb_role_switch	*role_sw;
	struct typec_partner	*partner;

	/* DisplayPort Alt Mode */
	struct typec_altmode	*dp_altmode;
	bool			dp_connected;
	int			dp_lanes;

	/* Power */
	struct regulator	*vbus_reg;
	struct power_supply	*usb_psy;
	int			pd_voltage_mv;
	int			pd_current_ma;

	/* Work / notifications */
	struct delayed_work	state_work;
	struct extcon_dev	*extcon;

	/* Child USB devices (set after enumeration) */
	struct usb_device	*hub_udev;
	struct usb_device	*lan_udev;
	struct usb_device	*sd_udev;
};

/* ----------------------------------------------------------------
 * Public API (used by platform glue / DT match)
 * ---------------------------------------------------------------- */

int  xiaomi_dock_probe(struct platform_device *pdev);
void xiaomi_dock_remove(struct platform_device *pdev);
int  xiaomi_dock_suspend(struct device *dev);
int  xiaomi_dock_resume(struct device *dev);

/* Notify the dock driver of a USB-C connection / disconnection event */
void xiaomi_dock_notify_connect(struct xiaomi_usb_dock *dock);
void xiaomi_dock_notify_disconnect(struct xiaomi_usb_dock *dock);

/* Query current dock state */
static inline bool xiaomi_dock_is_connected(struct xiaomi_usb_dock *dock)
{
	return dock->state == DOCK_STATE_CONNECTED;
}

static inline bool xiaomi_dock_has_dp(struct xiaomi_usb_dock *dock)
{
	return dock->capabilities & XIAOMI_DOCK_CAP_DP;
}

#endif /* __XIAOMI_USB_DOCK_H__ */
