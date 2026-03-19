// SPDX-License-Identifier: GPL-2.0-only
/*
 * xiaomi_usb_dock_pd.c - USB Power Delivery handling for Xiaomi USB-C Dock
 *
 * Manages PD negotiation between:
 *   1. External charger  →  dock USB-C power input port  (upstream)
 *   2. Dock              →  phone USB-C port             (downstream VBUS)
 *
 * Power flow modes:
 *   A. Dock-powered: charger ≥5 V connected to dock;
 *      dock negotiates a PD contract and feeds a regulated voltage to
 *      the phone VBUS pin while also powering its own peripherals.
 *   B. Phone-powered: no charger at dock;
 *      phone supplies up to 5 V / 900 mA via the USB-C cable to power
 *      dock hub + low-power peripherals only.  DP/HDMI/Ethernet remain
 *      functional; SD reader works at reduced speed.
 *
 * On-dock power management ICs (reference design):
 *   • FUSB302B  – USB PD policy engine + CC logic
 *   • TPS65987D – Integrated PD controller with PPS support
 *   • (or equivalent: CYPD3177, RT1716, HUSB238)
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/power_supply.h>
#include <linux/usb/pd.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include <linux/xiaomi-dock/xiaomi_usb_dock.h>

/* ----------------------------------------------------------------
 * PD Source Capabilities advertised by dock to the phone
 * (the dock acts as a PD source when powered by external charger)
 * ---------------------------------------------------------------- */

/*
 * PDO table (USB PD 3.0, Table 6-9):
 * The dock advertises these fixed supply PDOs to the phone.
 * Values chosen to be compatible with Qualcomm QC/VOOC and
 * Xiaomi HyperCharge (up to 67 W at 11 V / 6.1 A).
 */
#define DOCK_PDO_5V_3A		PDO_FIXED(5000,  3000, PDO_FIXED_FLAGS)
#define DOCK_PDO_9V_3A		PDO_FIXED(9000,  3000, PDO_FIXED_FLAGS)
#define DOCK_PDO_12V_3A		PDO_FIXED(12000, 3000, PDO_FIXED_FLAGS)
#define DOCK_PDO_15V_3A		PDO_FIXED(15000, 3000, PDO_FIXED_FLAGS)
#define DOCK_PDO_20V_3A25	PDO_FIXED(20000, 3250, PDO_FIXED_FLAGS)

/* Programmable Power Supply PDO (PPS): 3.3–21 V / 5 A */
#define DOCK_PDO_PPS		PDO_PPS_APDO(3300, 21000, 5000)

#define PDO_FIXED_FLAGS		(PDO_FIXED_DUAL_ROLE   | \
				 PDO_FIXED_DATA_SWAP   | \
				 PDO_FIXED_USB_COMM    | \
				 PDO_FIXED_EXTPOWER)

static const u32 dock_src_pdos[] = {
	DOCK_PDO_5V_3A,
	DOCK_PDO_9V_3A,
	DOCK_PDO_12V_3A,
	DOCK_PDO_15V_3A,
	DOCK_PDO_20V_3A25,
	DOCK_PDO_PPS,
};

/* ----------------------------------------------------------------
 * PD Sink Capabilities (dock consuming power from upstream charger)
 * ---------------------------------------------------------------- */

/*
 * When an upstream charger is connected to the dock's power input port,
 * the on-dock PD controller issues a Request Data Object (RDO) for the
 * highest power level the charger supports, up to 65 W.
 */

static const u32 dock_snk_pdos[] = {
	PDO_FIXED(5000,  900,  0),
	PDO_FIXED(9000,  3000, 0),
	PDO_FIXED(15000, 3000, 0),
	PDO_FIXED(20000, 3250, 0),
	PDO_PPS_APDO(3300, 21000, 5000),
};

/* ----------------------------------------------------------------
 * Power budget allocator
 *
 * Total input power is split between:
 *   • Dock self consumption:  max XIAOMI_DOCK_SELF_POWER_MW
 *   • Phone charging:         remainder (capped at dock_pd_max_mw)
 *
 * Example: charger supplies 65 W
 *   → dock keeps  5 W
 *   → phone gets 60 W  (12 V / 5 A or 20 V / 3 A, whichever phone accepts)
 * ---------------------------------------------------------------- */

struct dock_power_budget {
	int input_mw;      /* Total power from upstream charger */
	int self_mw;       /* Allocated to dock circuitry */
	int phone_mw;      /* Available for phone charging */
	int phone_mv;      /* Negotiated phone charging voltage */
	int phone_ma;      /* Negotiated phone charging current */
};

static void dock_compute_power_budget(struct xiaomi_usb_dock *dock,
				      int input_mw,
				      struct dock_power_budget *budget)
{
	budget->input_mw = input_mw;
	budget->self_mw  = min(XIAOMI_DOCK_SELF_POWER_MW, input_mw);
	budget->phone_mw = min(input_mw - budget->self_mw,
			       dock_pd_max_mw);

	/*
	 * Choose phone charging voltage:
	 * Prefer highest standard fixed PDO voltage that keeps current ≤ 5 A.
	 *   20 V @ 3.25 A = 65 W  → use if phone_mw ≥ 60000
	 *   15 V @ 4.00 A = 60 W  → use if phone_mw ≥ 45000
	 *   12 V @ 4.00 A = 48 W  → use if phone_mw ≥ 36000
	 *    9 V @ 3.00 A = 27 W  → use if phone_mw ≥ 18000
	 *    5 V @ 3.00 A = 15 W  → fallback
	 */
	if (budget->phone_mw >= 60000) {
		budget->phone_mv = XIAOMI_DOCK_PD_VOLT_20V;
		budget->phone_ma = budget->phone_mw / XIAOMI_DOCK_PD_VOLT_20V
				   * 1000;
	} else if (budget->phone_mw >= 45000) {
		budget->phone_mv = XIAOMI_DOCK_PD_VOLT_15V;
		budget->phone_ma = budget->phone_mw / XIAOMI_DOCK_PD_VOLT_15V
				   * 1000;
	} else if (budget->phone_mw >= 36000) {
		budget->phone_mv = XIAOMI_DOCK_PD_VOLT_12V;
		budget->phone_ma = budget->phone_mw / XIAOMI_DOCK_PD_VOLT_12V
				   * 1000;
	} else if (budget->phone_mw >= 18000) {
		budget->phone_mv = XIAOMI_DOCK_PD_VOLT_9V;
		budget->phone_ma = budget->phone_mw / XIAOMI_DOCK_PD_VOLT_9V
				   * 1000;
	} else {
		budget->phone_mv = XIAOMI_DOCK_PD_VOLT_5V;
		budget->phone_ma = budget->phone_mw / XIAOMI_DOCK_PD_VOLT_5V
				   * 1000;
	}

	dock->pd_voltage_mv = budget->phone_mv;
	dock->pd_current_ma = budget->phone_ma;

	dev_dbg(dock->dev,
		"PD budget: input=%d mW, self=%d mW, phone=%d mW (%d mV/%d mA)\n",
		budget->input_mw, budget->self_mw, budget->phone_mw,
		budget->phone_mv, budget->phone_ma);
}

/* ----------------------------------------------------------------
 * Power supply notifications (phone-side USB PSY)
 * ---------------------------------------------------------------- */

static int dock_psy_notifier_call(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	/*
	 * In a full implementation this would watch for PSY_EVENT_PROP_CHANGED
	 * on the phone's USB power_supply and re-negotiate PD if the phone
	 * requests a different voltage/current.
	 */
	return NOTIFY_OK;
}

/* ----------------------------------------------------------------
 * Module init / exit (PD sub-module)
 * ---------------------------------------------------------------- */

static int __init xiaomi_dock_pd_init(void)
{
	pr_debug("xiaomi-dock-pd: PD module loaded\n");
	return 0;
}

static void __exit xiaomi_dock_pd_exit(void)
{
	pr_debug("xiaomi-dock-pd: PD module unloaded\n");
}

module_init(xiaomi_dock_pd_init);
module_exit(xiaomi_dock_pd_exit);

MODULE_AUTHOR("Xiaomi USB Dock Team");
MODULE_DESCRIPTION("Xiaomi USB-C Dock Power Delivery Manager");
MODULE_LICENSE("GPL v2");
