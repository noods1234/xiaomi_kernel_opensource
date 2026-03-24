// SPDX-License-Identifier: GPL-2.0-only
/*
 * nd_controller.c — LC Variable ND Filter V4L2 subdev + USB CDC driver
 *
 * ONE INCH WONDER — Xiaomi 13U/14U IMX989 camera system
 * Document: LCDRV-KMOD-001 Rev A  |  2026-03-24
 *
 * Registers as a V4L2 subdev and communicates with the STM32G0B1
 * firmware (LCDRV-001 PCB) over USB CDC bulk endpoints.
 *
 * V4L2 controls exposed:
 *   nd_target_stops_x100   RW  — commanded ND attenuation (×100 stops)
 *   nd_actual_stops_x100   RO  — measured ND attenuation (×100 stops)
 *   nd_mode                RW  — 0=manual 1=auto-HAL 2=auto-MCU 3=bypass
 *   nd_cell_temp_decidegc  RO  — LC cell temperature (×10 °C)
 *   nd_settled             RO  — 1 when ND has settled to target
 *   nd_ccm_index           RO  — CCM table index for current ND level
 *   nd_cal_trigger         WO  — button: arms STM32 calibration sequence
 *
 * USB packet format (host→device and device→host):
 *   [cmd/reg : u8][val_lo : u8][val_hi : u8][checksum : u8 = XOR(0..2)]
 *
 * Kernel config requirements:
 *   CONFIG_VIDEO_V4L2_SUBDEV_API=y
 *   CONFIG_USB_CDC_ACM=y  (or m — nd_controller replaces it for this VID/PID)
 *   CONFIG_MEDIA_CONTROLLER=y
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mediabus.h>

/* ── Module metadata ─────────────────────────────────────────── */
MODULE_DESCRIPTION("LC Variable ND Filter V4L2 subdev (One Inch Wonder)");
MODULE_AUTHOR("One Inch Wonder project");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");

/* ── USB device identity ─────────────────────────────────────── */
/* NOTE: VID 0x0483 is the ST Microelectronics development VID.
 * Replace with an allocated VID/PID pair before production.        */
#define ND_USB_VID      0x0483
#define ND_USB_PID      0x5740

/* ── Custom V4L2 control IDs ─────────────────────────────────── */
#define V4L2_CID_ND_TARGET_STOPS_X100  (V4L2_CID_USER_BASE + 0x1000)
#define V4L2_CID_ND_ACTUAL_STOPS_X100  (V4L2_CID_USER_BASE + 0x1001)
#define V4L2_CID_ND_MODE               (V4L2_CID_USER_BASE + 0x1002)
#define V4L2_CID_ND_CELL_TEMP_DECIDEGC (V4L2_CID_USER_BASE + 0x1003)
#define V4L2_CID_ND_SETTLED            (V4L2_CID_USER_BASE + 0x1004)
#define V4L2_CID_ND_CCM_INDEX          (V4L2_CID_USER_BASE + 0x1005)
#define V4L2_CID_ND_CAL_TRIGGER        (V4L2_CID_USER_BASE + 0x1006)

/* ── STM32 USB command bytes (LCDRV-001 firmware protocol) ───── */
#define ND_CMD_WRITE_TARGET    0x10
#define ND_CMD_READ_ACTUAL     0x11
#define ND_CMD_WRITE_MODE      0x12
#define ND_CMD_READ_TEMP       0x13
#define ND_CMD_READ_STATUS     0x14
#define ND_CMD_READ_CCM_IDX    0x15
#define ND_CMD_PING            0xFF

/* ── STATUS register bit positions ──────────────────────────── */
#define ND_STATUS_SETTLED      BIT(0)
#define ND_STATUS_DC_FAULT     BIT(1)
#define ND_STATUS_CAL_ACTIVE   BIT(2)
#define ND_STATUS_LUT_VALID    BIT(3)
#define ND_STATUS_TEMP_WARN    BIT(4)
#define ND_STATUS_LC_OPEN      BIT(5)

/* ── Calibration magic bytes ─────────────────────────────────── */
#define ND_CAL_TRIGGER_MAGIC   0xCA

/* ── Timing constants ────────────────────────────────────────── */
#define ND_POLL_FAST_MS        33     /* ND_ACTUAL + STATUS poll interval  */
#define ND_TEMP_POLL_TICKS     15     /* temperature every 15 × 33 ms ≈ 500 ms */
#define ND_USB_TIMEOUT_MS      200    /* USB bulk transfer timeout          */

/* ── USB packet length ───────────────────────────────────────── */
#define ND_PKT_LEN             4

/* ── ND range constants (units: 0.01 stop) ───────────────────── */
#define ND_STOPS_MIN_X100      110    /* ND 2.1  (≈ ND2)  */
#define ND_STOPS_MAX_X100      700    /* ND 128             */
#define ND_STOPS_STEP_X100     10
#define ND_STOPS_DEF_X100      110

/* ─────────────────────────────────────────────────────────────── *
 * Per-device state                                                *
 * ─────────────────────────────────────────────────────────────── */

struct nd_ctrl_dev {
	struct usb_device        *udev;
	struct usb_interface     *intf;

	struct v4l2_device        v4l2_dev;
	struct v4l2_subdev        sd;
	struct v4l2_ctrl_handler  ctrl_handler;

	/* USB endpoints (numbers, not addresses) */
	unsigned int              bulk_out_ep;
	unsigned int              bulk_in_ep;

	/*
	 * Polled state cache.  Updated under usb_lock by the workqueue;
	 * read lock-free via READ_ONCE in g_volatile_ctrl (acceptable
	 * stale tolerance: one poll period ≈ 33 ms).
	 */
	s32                       nd_actual_x100;
	s32                       temp_decidegc;
	u8                        status_reg;
	s32                       ccm_index;

	struct mutex              usb_lock;    /* serialises all USB I/O  */
	bool                      disconnected;

	/* Background polling */
	struct delayed_work       poll_dwork;
	unsigned int              poll_tick;   /* counts fast-period ticks */
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
 * Returns 0 on success, negative errno on failure.
 * -EBADMSG is returned on checksum mismatch.
 */
static int nd_transact(struct nd_ctrl_dev *dev, u8 cmd,
		       u8 val_lo, u8 val_hi, u16 *resp_val)
{
	u8 tx[ND_PKT_LEN], rx[ND_PKT_LEN];
	int actual, ret;

	tx[0] = cmd;
	tx[1] = val_lo;
	tx[2] = val_hi;
	tx[3] = nd_checksum(tx, 3);

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_ep),
			   tx, ND_PKT_LEN, &actual, ND_USB_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(&dev->intf->dev, "TX error cmd=0x%02x: %d\n", cmd, ret);
		return ret;
	}

	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep),
			   rx, ND_PKT_LEN, &actual, ND_USB_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(&dev->intf->dev, "RX error cmd=0x%02x: %d\n", cmd, ret);
		return ret;
	}
	if (actual < ND_PKT_LEN) {
		dev_dbg(&dev->intf->dev, "Short RX: %d bytes\n", actual);
		return -EIO;
	}
	if (nd_checksum(rx, 3) != rx[3]) {
		dev_warn(&dev->intf->dev,
			 "Checksum mismatch on cmd=0x%02x (rx=%02x%02x%02x%02x)\n",
			 cmd, rx[0], rx[1], rx[2], rx[3]);
		return -EBADMSG;
	}

	if (resp_val)
		*resp_val = (u16)rx[1] | ((u16)rx[2] << 8);

	return 0;
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

	if (dev->disconnected)
		return -ENODEV;

	mutex_lock(&dev->usb_lock);

	switch (ctrl->id) {

	case V4L2_CID_ND_TARGET_STOPS_X100: {
		/*
		 * Encode: ND_TARGET_u8 = round((stops_x100 - 110) × 255 / 590)
		 * Inverse: stops_x100  = 110 + round(ND_TARGET_u8 × 590 / 255)
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
		ret = nd_write_reg(dev, ND_CMD_WRITE_TARGET,
				   ND_CAL_TRIGGER_MAGIC);
		/* re-use 0x10 target command slot; firmware expects 0xCA */
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
 * g_volatile_ctrl — returns values from the workqueue-maintained cache.
 *
 * The workqueue polls the STM32 at ND_POLL_FAST_MS (33 ms) and at
 * ND_POLL_FAST_MS × ND_TEMP_POLL_TICKS (≈ 500 ms) for temperature.
 * Reading from the cache here avoids taking usb_lock inside the
 * V4L2 ctrl lock and eliminates a potential deadlock.
 */
static int nd_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct nd_ctrl_dev *dev =
		container_of(ctrl->handler, struct nd_ctrl_dev, ctrl_handler);

	if (dev->disconnected)
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
	/* LC cell power is always on while USB is connected. */
	return 0;
}

static int nd_s_stream(struct v4l2_subdev *sd, int enable)
{
	/* Optical path is passive; no stream gating required. */
	return 0;
}

/*
 * nd_get_fmt — passthrough subdev; format is set by the sensor behind it.
 * Returns sensible defaults so pad format queries don't fail.
 * Adjust IMX989 dimensions via DQ_4 (see INTEGRATION_MAP.md).
 */
static int nd_get_fmt(struct v4l2_subdev *sd,
		      struct v4l2_subdev_state *state,
		      struct v4l2_subdev_format *fmt)
{
	fmt->format.width  = 9248;   /* IMX989 full-res width  (verify DQ_4) */
	fmt->format.height = 6944;   /* IMX989 full-res height (verify DQ_4) */
	fmt->format.code   = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.field  = V4L2_FIELD_NONE;
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
};

static const struct v4l2_subdev_ops nd_subdev_ops = {
	.core  = &nd_core_ops,
	.video = &nd_video_ops,
	.pad   = &nd_pad_ops,
};

/* ─────────────────────────────────────────────────────────────── *
 * Background polling workqueue                                   *
 *                                                                 *
 * Fast tick  (33 ms):  ND_ACTUAL, STATUS, CCM_IDX               *
 * Slow tick (~500 ms): TEMP_C (every ND_TEMP_POLL_TICKS ticks)  *
 * ─────────────────────────────────────────────────────────────── */

static void nd_poll_work_fn(struct work_struct *work)
{
	struct nd_ctrl_dev *dev =
		container_of(work, struct nd_ctrl_dev, poll_dwork.work);
	u16 raw;
	s32 stops;

	if (dev->disconnected)
		return;

	mutex_lock(&dev->usb_lock);

	/* ── Fast reads ───────────────────────────────────────── */
	if (!nd_read_reg(dev, ND_CMD_READ_ACTUAL, &raw)) {
		stops = 110 + DIV_ROUND_CLOSEST((s32)raw * 590, 255);
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

	/* ── Fault / warning logging (rate-limited) ───────────── */
	if (READ_ONCE(dev->status_reg) & ND_STATUS_DC_FAULT)
		dev_warn_ratelimited(&dev->intf->dev,
				     "DC fault on LC cell — cell drive disabled by MCU\n");
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
		dev_err(&intf->dev,
			"Bulk IN/OUT endpoints not found — wrong interface?\n");
		return -ENODEV;
	}
	dev_dbg(&intf->dev, "Bulk IN ep=%u  OUT ep=%u\n",
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
		.min  = 0,
		.max  = 3,
		.step = 1,
		.def  = 0,
	};
	/* Temperature: units 0.1 °C; 245 = 24.5 °C.  Range −5…80 °C. */
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

	dev->udev             = usb_get_dev(udev);
	dev->intf             = intf;
	dev->nd_actual_x100   = ND_STOPS_DEF_X100;
	dev->temp_decidegc    = 250;   /* 25.0 °C safe default */
	mutex_init(&dev->usb_lock);

	ret = nd_find_endpoints(dev, intf);
	if (ret)
		goto err_put_dev;

	/* ── Firmware handshake ───────────────────────────────── */
	mutex_lock(&dev->usb_lock);
	ret = nd_transact(dev, ND_CMD_PING, 0, 0, &fw_ver);
	mutex_unlock(&dev->usb_lock);
	if (ret) {
		dev_err(&intf->dev,
			"STM32 PING failed (%d) — is LCDRV-001 firmware running?\n",
			ret);
		goto err_put_dev;
	}
	dev_info(&intf->dev, "STM32 firmware v%u.%u\n",
		 (fw_ver >> 8) & 0xFF, fw_ver & 0xFF);

	/* ── V4L2 device ─────────────────────────────────────── */
	ret = v4l2_device_register(&intf->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&intf->dev, "v4l2_device_register: %d\n", ret);
		goto err_put_dev;
	}

	/* ── V4L2 subdev ─────────────────────────────────────── */
	v4l2_subdev_init(&dev->sd, &nd_subdev_ops);
	dev->sd.owner = THIS_MODULE;
	dev->sd.dev   = &intf->dev;
	strscpy(dev->sd.name, "nd_controller", sizeof(dev->sd.name));

	ret = nd_register_controls(dev);
	if (ret)
		goto err_v4l2_unreg;

	ret = v4l2_device_register_subdev(&dev->v4l2_dev, &dev->sd);
	if (ret) {
		dev_err(&intf->dev, "v4l2_device_register_subdev: %d\n", ret);
		goto err_ctrl_free;
	}

	usb_set_intfdata(intf, dev);

	/* ── Start background polling ────────────────────────── */
	INIT_DELAYED_WORK(&dev->poll_dwork, nd_poll_work_fn);
	schedule_delayed_work(&dev->poll_dwork,
			      msecs_to_jiffies(ND_POLL_FAST_MS));

	dev_info(&intf->dev,
		 "LC ND controller ready — V4L2 subdev \"%s\" (poll %d ms)\n",
		 dev->sd.name, ND_POLL_FAST_MS);
	return 0;

err_ctrl_free:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
err_v4l2_unreg:
	v4l2_device_unregister(&dev->v4l2_dev);
err_put_dev:
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
	cancel_delayed_work_sync(&dev->poll_dwork);

	v4l2_device_unregister_subdev(&dev->sd);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister(&dev->v4l2_dev);

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
	.id_table   = nd_usb_table,
};

module_usb_driver(nd_usb_driver);
