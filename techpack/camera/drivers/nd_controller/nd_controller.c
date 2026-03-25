// SPDX-License-Identifier: GPL-2.0-only
/*
 * nd_controller.c — LC Variable ND Filter V4L2 subdev + USB CDC driver
 *
 * ONE INCH WONDER — Xiaomi 13U/14U IMX989 camera system
 * Document: LCDRV-KMOD-001 Rev B  |  2026-03-25
 *
 * Rev B changes from Rev A (drivers/media/usb/nd_controller.c):
 *  1. Relocated to techpack/camera/drivers/nd_controller/ (Xiaomi build tree)
 *  2. Drop embedded v4l2_device.  Registers via v4l2_async_register_subdev()
 *     so the Qualcomm camera service discovers this subdev through the shared
 *     media graph instead of through a private v4l2_device island.
 *  3. media_entity_pads_init(): 1 SOURCE pad — makes the ND filter visible
 *     as a node in the media pipeline (lens → nd_controller → IMX989).
 *  4. pm_runtime: enabled at probe; USB transactions hold a runtime PM ref.
 *     USB suspend cancels the polling workqueue; resume restarts it.
 *  5. USB suspend / resume ops added to usb_driver.
 *  6. Android.mk + Kconfig + Makefile wired into techpack build system.
 *
 * V4L2 controls exposed (CIDs defined in nd_controller.h):
 *   nd_target_stops_x100   RW  — commanded ND attenuation (×100 stops)
 *   nd_actual_stops_x100   RO  — measured ND attenuation (×100 stops)
 *   nd_mode                RW  — 0=manual 1=auto-HAL 2=auto-MCU 3=bypass
 *   nd_cell_temp_decidegc  RO  — LC cell temperature (×10 °C)
 *   nd_settled             RO  — 1 when ND has settled to target
 *   nd_ccm_index           RO  — CCM table index for current ND level
 *   nd_cal_trigger         WO  — button: arms STM32 calibration sequence
 *
 * USB packet format (host→device and device→host):
 *   [cmd : u8][val_lo : u8][val_hi : u8][checksum : u8 = XOR(0..2)]
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mediabus.h>

#include "nd_controller.h"

/* ── Module metadata ─────────────────────────────────────────── */
MODULE_DESCRIPTION("LC Variable ND Filter V4L2 subdev (One Inch Wonder)");
MODULE_AUTHOR("One Inch Wonder project");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("2.0");

/* ── STM32 USB command bytes (LCDRV-001 firmware protocol) ───── */
#define ND_CMD_WRITE_TARGET	0x10
#define ND_CMD_READ_ACTUAL	0x11
#define ND_CMD_WRITE_MODE	0x12
#define ND_CMD_READ_TEMP	0x13
#define ND_CMD_READ_STATUS	0x14
#define ND_CMD_READ_CCM_IDX	0x15
#define ND_CMD_PING		0xFF

/* ── STATUS register bit positions ──────────────────────────── */
#define ND_STATUS_SETTLED	BIT(0)
#define ND_STATUS_DC_FAULT	BIT(1)
#define ND_STATUS_CAL_ACTIVE	BIT(2)
#define ND_STATUS_LUT_VALID	BIT(3)
#define ND_STATUS_TEMP_WARN	BIT(4)
#define ND_STATUS_LC_OPEN	BIT(5)

/* ── Calibration magic byte ──────────────────────────────────── */
#define ND_CAL_TRIGGER_MAGIC	0xCA

/* ── Timing constants ────────────────────────────────────────── */
#define ND_POLL_FAST_MS		33	/* ND_ACTUAL + STATUS + CCM poll   */
#define ND_TEMP_POLL_TICKS	15	/* temperature every 15×33 ms ≈ 500 ms */
#define ND_USB_TIMEOUT_MS	200	/* USB bulk transfer timeout        */

/* ── USB packet length ───────────────────────────────────────── */
#define ND_PKT_LEN		4

/* ── pm_runtime autosuspend delay ────────────────────────────── */
#define ND_AUTOSUSPEND_DELAY_MS	2000

/* ─────────────────────────────────────────────────────────────── *
 * Per-device state                                                *
 * ─────────────────────────────────────────────────────────────── */

struct nd_ctrl_dev {
	struct usb_device	*udev;
	struct usb_interface	*intf;

	/*
	 * Rev B: no embedded v4l2_device.  The subdev is registered
	 * asynchronously and parented to the Qualcomm ISP v4l2_device
	 * during pipeline setup.
	 */
	struct v4l2_subdev	 sd;
	struct media_pad	 pad;		/* ND_PAD_SOURCE */
	struct v4l2_ctrl_handler ctrl_handler;

	/* USB endpoints (numbers, not addresses) */
	unsigned int		 bulk_out_ep;
	unsigned int		 bulk_in_ep;

	/*
	 * Polled state cache.  Written under usb_lock by the workqueue;
	 * read lock-free via READ_ONCE in g_volatile_ctrl.  One poll
	 * period (33 ms) of staleness is acceptable.
	 */
	s32			 nd_actual_x100;
	s32			 temp_decidegc;
	u8			 status_reg;
	s32			 ccm_index;

	struct mutex		 usb_lock;	/* serialises all USB I/O */
	bool			 disconnected;

	/* Background polling */
	struct delayed_work	 poll_dwork;
	unsigned int		 poll_tick;
};

/* ─────────────────────────────────────────────────────────────── *
 * USB packet helpers                                              *
 * ─────────────────────────────────────────────────────────────── */

static u8 nd_checksum(const u8 *buf, int n)
{
	u8 x = 0;
	int i;

	for (i = 0; i < n; i++)
		x ^= buf[i];
	return x;
}

/**
 * nd_transact() - send one command packet and receive one response.
 * @dev:      device context (caller must hold dev->usb_lock)
 * @cmd:      command byte
 * @val_lo:   low byte of the value field (use 0 for reads)
 * @val_hi:   high byte of the value field (use 0 for reads)
 * @resp_val: out — little-endian 16-bit value from response (may be NULL)
 *
 * Acquires a pm_runtime reference for the duration of the transfer.
 * Returns 0 on success, negative errno on failure.
 * -EBADMSG is returned on checksum mismatch.
 */
static int nd_transact(struct nd_ctrl_dev *dev, u8 cmd,
		       u8 val_lo, u8 val_hi, u16 *resp_val)
{
	struct device *d = &dev->intf->dev;
	u8 tx[ND_PKT_LEN], rx[ND_PKT_LEN];
	int actual, ret;

	ret = pm_runtime_get_sync(d);
	if (ret < 0) {
		pm_runtime_put_noidle(d);
		return ret;
	}

	tx[0] = cmd;
	tx[1] = val_lo;
	tx[2] = val_hi;
	tx[3] = nd_checksum(tx, 3);

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_ep),
			   tx, ND_PKT_LEN, &actual, ND_USB_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(d, "TX error cmd=0x%02x: %d\n", cmd, ret);
		goto out_put;
	}

	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep),
			   rx, ND_PKT_LEN, &actual, ND_USB_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(d, "RX error cmd=0x%02x: %d\n", cmd, ret);
		goto out_put;
	}
	if (actual < ND_PKT_LEN) {
		dev_dbg(d, "Short RX: %d bytes\n", actual);
		ret = -EIO;
		goto out_put;
	}
	if (nd_checksum(rx, 3) != rx[3]) {
		dev_warn(d, "Checksum mismatch cmd=0x%02x (rx=%02x%02x%02x%02x)\n",
			 cmd, rx[0], rx[1], rx[2], rx[3]);
		ret = -EBADMSG;
		goto out_put;
	}

	if (resp_val)
		*resp_val = (u16)rx[1] | ((u16)rx[2] << 8);
	ret = 0;

out_put:
	pm_runtime_mark_last_busy(d);
	pm_runtime_put_autosuspend(d);
	return ret;
}

static int nd_write_reg(struct nd_ctrl_dev *dev, u8 cmd, u16 val)
{
	return nd_transact(dev, cmd, val & 0xFF, (val >> 8) & 0xFF, NULL);
}

static int nd_read_reg(struct nd_ctrl_dev *dev, u8 cmd, u16 *out)
{
	return nd_transact(dev, cmd, 0, 0, out);
}

/* ─────────────────────────────────────────────────────────────── *
 * V4L2 control ops                                               *
 * ─────────────────────────────────────────────────────────────── */

static int nd_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct nd_ctrl_dev *dev =
		container_of(ctrl->handler, struct nd_ctrl_dev, ctrl_handler);
	int ret;

	if (READ_ONCE(dev->disconnected))
		return -ENODEV;

	mutex_lock(&dev->usb_lock);

	switch (ctrl->id) {

	case V4L2_CID_ND_TARGET_STOPS_X100: {
		/*
		 * Encode: ND_TARGET_u8 = round((stops_x100 - 110) × 255 / 590)
		 * Inverse: stops_x100 = 110 + round(ND_TARGET_u8 × 590 / 255)
		 */
		int raw = DIV_ROUND_CLOSEST((ctrl->val - 110) * 255, 590);

		raw = clamp(raw, 0, 255);
		ret = nd_write_reg(dev, ND_CMD_WRITE_TARGET, (u16)raw);
		break;
	}

	case V4L2_CID_ND_MODE:
		ret = nd_write_reg(dev, ND_CMD_WRITE_MODE, (u16)ctrl->val);
		break;

	case V4L2_CID_ND_CAL_TRIGGER:
		ret = nd_transact(dev, 0x20, ND_CAL_TRIGGER_MAGIC, 0, NULL);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&dev->usb_lock);
	return ret;
}

/*
 * nd_g_volatile_ctrl — returns values from the workqueue cache.
 *
 * Reads are lock-free (READ_ONCE).  The workqueue takes usb_lock before
 * writing the cache via WRITE_ONCE, so there is no race with nd_s_ctrl.
 * Staleness tolerance: one poll period ≈ 33 ms.
 */
static int nd_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct nd_ctrl_dev *dev =
		container_of(ctrl->handler, struct nd_ctrl_dev, ctrl_handler);

	if (READ_ONCE(dev->disconnected))
		return -ENODEV;

	switch (ctrl->id) {
	case V4L2_CID_ND_ACTUAL_STOPS_X100:
		ctrl->val = READ_ONCE(dev->nd_actual_x100);
		break;
	case V4L2_CID_ND_CELL_TEMP_DECIDEGC:
		ctrl->val = READ_ONCE(dev->temp_decidegc);
		break;
	case V4L2_CID_ND_SETTLED:
		ctrl->val = !!(READ_ONCE(dev->status_reg) & ND_STATUS_SETTLED);
		break;
	case V4L2_CID_ND_CCM_INDEX:
		ctrl->val = READ_ONCE(dev->ccm_index);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct v4l2_ctrl_ops nd_ctrl_ops = {
	.s_ctrl          = nd_s_ctrl,
	.g_volatile_ctrl = nd_g_volatile_ctrl,
};

/* ─────────────────────────────────────────────────────────────── *
 * V4L2 subdev ops                                                *
 * ─────────────────────────────────────────────────────────────── */

static int nd_s_power(struct v4l2_subdev *sd, int on)
{
	/*
	 * LC cell drive is always active while USB is connected.
	 * The USB pm_runtime path handles device-level power.
	 */
	return 0;
}

static int nd_s_stream(struct v4l2_subdev *sd, int enable)
{
	/* Optical attenuator — no stream gating needed. */
	return 0;
}

/*
 * nd_get_fmt — passthrough filter; format is determined by the IMX989.
 * Returns sensible defaults so VIDIOC_SUBDEV_G_FMT doesn't fail.
 * Verify exact active area dimensions against DQ_4 (INTEGRATION_MAP.md).
 */
static int nd_get_fmt(struct v4l2_subdev *sd,
		      struct v4l2_subdev_state *state,
		      struct v4l2_subdev_format *fmt)
{
	fmt->format.width  = 9248;		/* IMX989 full-res width  */
	fmt->format.height = 6944;		/* IMX989 full-res height */
	fmt->format.code   = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.field  = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	return 0;
}

static const struct v4l2_subdev_core_ops nd_core_ops = {
	.s_power = nd_s_power,
};

static const struct v4l2_subdev_video_ops nd_video_ops = {
	.s_stream = nd_s_stream,
};

static const struct v4l2_subdev_pad_ops nd_pad_ops = {
	.get_fmt = nd_get_fmt,
	/*
	 * set_fmt is intentionally absent: the ND filter is optical only.
	 * Format is owned by the IMX989; we never reconfigure it.
	 */
};

static const struct v4l2_subdev_ops nd_subdev_ops = {
	.core  = &nd_core_ops,
	.video = &nd_video_ops,
	.pad   = &nd_pad_ops,
};

/* ─────────────────────────────────────────────────────────────── *
 * Background polling workqueue                                   *
 *                                                                 *
 * Fast tick (33 ms):   ND_ACTUAL, STATUS, CCM_IDX               *
 * Slow tick (~500 ms): TEMP (every ND_TEMP_POLL_TICKS ticks)    *
 *                                                                 *
 * The workqueue is cancelled by nd_usb_suspend() and restarted   *
 * by nd_usb_resume() so it does not fire during USB suspend.     *
 * ─────────────────────────────────────────────────────────────── */

static void nd_poll_work_fn(struct work_struct *work)
{
	struct nd_ctrl_dev *dev =
		container_of(work, struct nd_ctrl_dev, poll_dwork.work);
	u16 raw;
	s32 stops;

	if (READ_ONCE(dev->disconnected))
		return;

	mutex_lock(&dev->usb_lock);

	/* ── Fast reads ───────────────────────────────────────── */
	if (!nd_read_reg(dev, ND_CMD_READ_ACTUAL, &raw)) {
		stops = ND_STOPS_MIN_X100 +
			DIV_ROUND_CLOSEST((s32)raw * 590, 255);
		WRITE_ONCE(dev->nd_actual_x100,
			   clamp(stops, ND_STOPS_MIN_X100, ND_STOPS_MAX_X100));
	}

	if (!nd_read_reg(dev, ND_CMD_READ_STATUS, &raw))
		WRITE_ONCE(dev->status_reg, (u8)raw);

	if (!nd_read_reg(dev, ND_CMD_READ_CCM_IDX, &raw))
		WRITE_ONCE(dev->ccm_index, (s32)(raw & 0xFF));

	/* ── Slow read: temperature ───────────────────────────── */
	if (++dev->poll_tick >= ND_TEMP_POLL_TICKS) {
		dev->poll_tick = 0;
		if (!nd_read_reg(dev, ND_CMD_READ_TEMP, &raw))
			WRITE_ONCE(dev->temp_decidegc, (s32)(s16)raw);
	}

	mutex_unlock(&dev->usb_lock);

	/* ── Rate-limited fault logging ───────────────────────── */
	if (READ_ONCE(dev->status_reg) & ND_STATUS_DC_FAULT)
		dev_warn_ratelimited(&dev->intf->dev,
				     "DC fault — LC cell drive disabled by MCU\n");
	if (READ_ONCE(dev->status_reg) & ND_STATUS_TEMP_WARN)
		dev_warn_ratelimited(&dev->intf->dev,
				     "LC cell temperature warning (>40 °C)\n");
	if (READ_ONCE(dev->status_reg) & ND_STATUS_LC_OPEN)
		dev_warn_ratelimited(&dev->intf->dev,
				     "LC cell open-circuit fault\n");

	schedule_delayed_work(&dev->poll_dwork,
			      msecs_to_jiffies(ND_POLL_FAST_MS));
}

/* ─────────────────────────────────────────────────────────────── *
 * pm_runtime ops (USB-level suspend / resume)                    *
 * ─────────────────────────────────────────────────────────────── */

static int nd_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct nd_ctrl_dev *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;

	/*
	 * Cancel polling before the USB bus goes to sleep.
	 * Any in-flight transaction inside nd_poll_work_fn will complete
	 * first because cancel_delayed_work_sync() waits for it.
	 */
	cancel_delayed_work_sync(&dev->poll_dwork);
	dev_dbg(&intf->dev, "ND controller suspended\n");
	return 0;
}

static int nd_usb_resume(struct usb_interface *intf)
{
	struct nd_ctrl_dev *dev = usb_get_intfdata(intf);

	if (!dev || READ_ONCE(dev->disconnected))
		return 0;

	schedule_delayed_work(&dev->poll_dwork,
			      msecs_to_jiffies(ND_POLL_FAST_MS));
	dev_dbg(&intf->dev, "ND controller resumed\n");
	return 0;
}

/* ─────────────────────────────────────────────────────────────── *
 * Endpoint discovery                                             *
 * ─────────────────────────────────────────────────────────────── */

static int nd_find_endpoints(struct nd_ctrl_dev *dev,
			     struct usb_interface *intf)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep;
	int i;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_bulk_in(ep) && !dev->bulk_in_ep)
			dev->bulk_in_ep = usb_endpoint_num(ep);
		else if (usb_endpoint_is_bulk_out(ep) && !dev->bulk_out_ep)
			dev->bulk_out_ep = usb_endpoint_num(ep);
	}

	if (!dev->bulk_in_ep || !dev->bulk_out_ep) {
		dev_err(&intf->dev, "Bulk IN/OUT endpoints not found\n");
		return -ENODEV;
	}
	dev_dbg(&intf->dev, "Bulk IN ep=%u OUT ep=%u\n",
		dev->bulk_in_ep, dev->bulk_out_ep);
	return 0;
}

/* ─────────────────────────────────────────────────────────────── *
 * V4L2 control registration                                      *
 * ─────────────────────────────────────────────────────────────── */

static int nd_register_controls(struct nd_ctrl_dev *dev)
{
	struct v4l2_ctrl_handler *hdl = &dev->ctrl_handler;

	static const struct v4l2_ctrl_config cfg_target = {
		.ops  = &nd_ctrl_ops,
		.id   = V4L2_CID_ND_TARGET_STOPS_X100,
		.name = "nd_target_stops_x100",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min  = ND_STOPS_MIN_X100,
		.max  = ND_STOPS_MAX_X100,
		.step = ND_STOPS_STEP_X100,
		.def  = ND_STOPS_DEF_X100,
	};
	static const struct v4l2_ctrl_config cfg_actual = {
		.ops   = &nd_ctrl_ops,
		.id    = V4L2_CID_ND_ACTUAL_STOPS_X100,
		.name  = "nd_actual_stops_x100",
		.type  = V4L2_CTRL_TYPE_INTEGER,
		.min   = ND_STOPS_MIN_X100,
		.max   = ND_STOPS_MAX_X100,
		.step  = ND_STOPS_STEP_X100,
		.def   = ND_STOPS_DEF_X100,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	};
	static const struct v4l2_ctrl_config cfg_mode = {
		.ops  = &nd_ctrl_ops,
		.id   = V4L2_CID_ND_MODE,
		.name = "nd_mode",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min  = 0,	/* manual  */
		.max  = 3,	/* bypass  */
		.step = 1,
		.def  = 0,
	};
	/* Temperature: units 0.1 °C; 250 = 25.0 °C.  Range −5…80 °C. */
	static const struct v4l2_ctrl_config cfg_temp = {
		.ops   = &nd_ctrl_ops,
		.id    = V4L2_CID_ND_CELL_TEMP_DECIDEGC,
		.name  = "nd_cell_temp_decidegc",
		.type  = V4L2_CTRL_TYPE_INTEGER,
		.min   = -50,
		.max   = 800,
		.step  = 1,
		.def   = 250,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	};
	static const struct v4l2_ctrl_config cfg_settled = {
		.ops   = &nd_ctrl_ops,
		.id    = V4L2_CID_ND_SETTLED,
		.name  = "nd_settled",
		.type  = V4L2_CTRL_TYPE_BOOLEAN,
		.min   = 0,
		.max   = 1,
		.step  = 1,
		.def   = 0,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	};
	static const struct v4l2_ctrl_config cfg_ccm = {
		.ops   = &nd_ctrl_ops,
		.id    = V4L2_CID_ND_CCM_INDEX,
		.name  = "nd_ccm_index",
		.type  = V4L2_CTRL_TYPE_INTEGER,
		.min   = 0,
		.max   = 60,
		.step  = 1,
		.def   = 0,
		.flags = V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_VOLATILE,
	};
	static const struct v4l2_ctrl_config cfg_cal = {
		.ops  = &nd_ctrl_ops,
		.id   = V4L2_CID_ND_CAL_TRIGGER,
		.name = "nd_cal_trigger",
		.type = V4L2_CTRL_TYPE_BUTTON,
		.min  = 0,
		.max  = 0,
		.step = 1,
		.def  = 0,
	};

	v4l2_ctrl_handler_init(hdl, 7);

	v4l2_ctrl_new_custom(hdl, &cfg_target,  NULL);
	v4l2_ctrl_new_custom(hdl, &cfg_actual,  NULL);
	v4l2_ctrl_new_custom(hdl, &cfg_mode,    NULL);
	v4l2_ctrl_new_custom(hdl, &cfg_temp,    NULL);
	v4l2_ctrl_new_custom(hdl, &cfg_settled, NULL);
	v4l2_ctrl_new_custom(hdl, &cfg_ccm,     NULL);
	v4l2_ctrl_new_custom(hdl, &cfg_cal,     NULL);

	if (hdl->error) {
		dev_err(&dev->intf->dev,
			"Control handler init failed: %d\n", hdl->error);
		v4l2_ctrl_handler_free(hdl);
		return hdl->error;
	}

	dev->sd.ctrl_handler = hdl;
	return 0;
}

/* ─────────────────────────────────────────────────────────────── *
 * USB probe / disconnect                                          *
 * ─────────────────────────────────────────────────────────────── */

static int nd_probe(struct usb_interface *intf,
		    const struct usb_device_id *id)
{
	struct usb_device  *udev = interface_to_usbdev(intf);
	struct nd_ctrl_dev *dev;
	u16 fw_ver = 0;
	int ret;

	/*
	 * The STM32 CDC device has two interfaces:
	 *   0 — CDC control  (bInterfaceClass 0x02, no bulk endpoints)
	 *   1 — CDC data     (bInterfaceClass 0x0A, bulk IN + OUT)
	 * Only bind to the data interface.
	 */
	if (intf->cur_altsetting->desc.bInterfaceClass != USB_CLASS_CDC_DATA)
		return -ENODEV;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->udev          = usb_get_dev(udev);
	dev->intf          = intf;
	dev->nd_actual_x100 = ND_STOPS_DEF_X100;
	dev->temp_decidegc  = 250;	/* 25.0 °C */
	mutex_init(&dev->usb_lock);
	usb_set_intfdata(intf, dev);

	/* ── pm_runtime — enable before any USB transaction ──── */
	pm_runtime_set_active(&intf->dev);
	pm_runtime_set_autosuspend_delay(&intf->dev, ND_AUTOSUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&intf->dev);
	pm_runtime_enable(&intf->dev);

	ret = nd_find_endpoints(dev, intf);
	if (ret)
		goto err_pm_disable;

	/* ── Firmware handshake ───────────────────────────────── */
	mutex_lock(&dev->usb_lock);
	ret = nd_transact(dev, ND_CMD_PING, 0, 0, &fw_ver);
	mutex_unlock(&dev->usb_lock);
	if (ret) {
		dev_err(&intf->dev,
			"STM32 PING failed (%d) — is LCDRV-001 firmware running?\n",
			ret);
		goto err_pm_disable;
	}
	dev_info(&intf->dev, "STM32 firmware v%u.%u\n",
		 (fw_ver >> 8) & 0xFF, fw_ver & 0xFF);

	/* ── V4L2 subdev ─────────────────────────────────────── */
	v4l2_subdev_init(&dev->sd, &nd_subdev_ops);
	dev->sd.owner  = THIS_MODULE;
	dev->sd.dev    = &intf->dev;
	dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(dev->sd.name, ND_SUBDEV_NAME, sizeof(dev->sd.name));
	v4l2_set_subdevdata(&dev->sd, dev);

	/* ── Media entity — 1 SOURCE pad ─────────────────────── *
	 * The ND filter sits in the optical path in front of the  *
	 * IMX989.  A single SOURCE pad exposes it as a node in    *
	 * the media pipeline (pad link to IMX989 SINK is set up   *
	 * by the board-level camera platform driver).             */
	dev->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&dev->sd.entity, ND_NUM_PADS, &dev->pad);
	if (ret) {
		dev_err(&intf->dev, "media_entity_pads_init: %d\n", ret);
		goto err_pm_disable;
	}
	dev->sd.entity.function = MEDIA_ENT_F_LENS;

	ret = nd_register_controls(dev);
	if (ret)
		goto err_media_cleanup;

	/*
	 * v4l2_async_register_subdev() makes this subdev discoverable
	 * to the Qualcomm camera service via the media graph.  The
	 * V4L2 async framework will call the master notifier's
	 * bound() callback when the ISP's v4l2_device is ready.
	 */
	ret = v4l2_async_register_subdev(&dev->sd);
	if (ret) {
		dev_err(&intf->dev, "v4l2_async_register_subdev: %d\n", ret);
		goto err_ctrl_free;
	}

	/* ── Start background polling ────────────────────────── */
	INIT_DELAYED_WORK(&dev->poll_dwork, nd_poll_work_fn);
	schedule_delayed_work(&dev->poll_dwork,
			      msecs_to_jiffies(ND_POLL_FAST_MS));

	dev_info(&intf->dev,
		 "LC ND controller ready — subdev \"%s\" (poll %d ms)\n",
		 dev->sd.name, ND_POLL_FAST_MS);
	return 0;

err_ctrl_free:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
err_media_cleanup:
	media_entity_cleanup(&dev->sd.entity);
err_pm_disable:
	pm_runtime_disable(&intf->dev);
	usb_set_intfdata(intf, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

static void nd_disconnect(struct usb_interface *intf)
{
	struct nd_ctrl_dev *dev = usb_get_intfdata(intf);

	if (!dev)
		return;

	WRITE_ONCE(dev->disconnected, true);

	/* Stop polling before tearing down USB and V4L2 state. */
	cancel_delayed_work_sync(&dev->poll_dwork);

	v4l2_async_unregister_subdev(&dev->sd);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	media_entity_cleanup(&dev->sd.entity);

	pm_runtime_disable(&intf->dev);

	usb_set_intfdata(intf, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);

	dev_info(&intf->dev, "LC ND controller disconnected\n");
}

/* ─────────────────────────────────────────────────────────────── *
 * USB driver registration                                        *
 * ─────────────────────────────────────────────────────────────── */

static const struct usb_device_id nd_usb_table[] = {
	{ USB_DEVICE(ND_USB_VID, ND_USB_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, nd_usb_table);

static struct usb_driver nd_usb_driver = {
	.name       = "nd_controller",
	.probe      = nd_probe,
	.disconnect = nd_disconnect,
	.suspend    = nd_usb_suspend,
	.resume     = nd_usb_resume,
	.reset_resume = nd_usb_resume,	/* recover from USB reset */
	.id_table   = nd_usb_table,
	.supports_autosuspend = 1,
};

module_usb_driver(nd_usb_driver);
