/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nd_controller.h — Public interface for the LC ND filter kernel driver
 *
 * ONE INCH WONDER — Xiaomi 13U/14U IMX989 camera system
 * Document: LCDRV-KMOD-001 Rev B  |  2026-03-25
 *
 * This header is shared between the driver and any kernel code that needs
 * to locate or reference the nd_controller subdev (e.g. a platform glue
 * layer that creates a media pipeline link to the IMX989 entity).
 */

#ifndef _ND_CONTROLLER_H
#define _ND_CONTROLLER_H

#include <linux/types.h>
#include <media/v4l2-subdev.h>

/* ── USB device identity ─────────────────────────────────────── *
 * VID 0x0483: STMicroelectronics (development allocation).       *
 * PID 0x5750: reserved for LCDRV-001 within the OIW project.    *
 * Replace with an allocated VID/PID before production.           */
#define ND_USB_VID		0x0483
#define ND_USB_PID		0x5750

/* ── V4L2 subdev name (matches what HAL scans for) ───────────── */
#define ND_SUBDEV_NAME		"nd_controller"

/* ── Custom V4L2 control IDs ─────────────────────────────────── *
 * V4L2_CID_USER_BASE = V4L2_CTRL_CLASS_USER | 0x900 = 0x00980900 *
 * These offsets must mirror nd_v4l2_client.h in the HAL layer.   */
#define V4L2_CID_ND_TARGET_STOPS_X100	(V4L2_CID_USER_BASE + 0x1000)
#define V4L2_CID_ND_ACTUAL_STOPS_X100	(V4L2_CID_USER_BASE + 0x1001)
#define V4L2_CID_ND_MODE		(V4L2_CID_USER_BASE + 0x1002)
#define V4L2_CID_ND_CELL_TEMP_DECIDEGC	(V4L2_CID_USER_BASE + 0x1003)
#define V4L2_CID_ND_SETTLED		(V4L2_CID_USER_BASE + 0x1004)
#define V4L2_CID_ND_CCM_INDEX		(V4L2_CID_USER_BASE + 0x1005)
#define V4L2_CID_ND_CAL_TRIGGER		(V4L2_CID_USER_BASE + 0x1006)

/* ── ND range constants (units: 0.01 stop) ───────────────────── */
#define ND_STOPS_MIN_X100	110	/* ND 2.1  (≈ ND2)  */
#define ND_STOPS_MAX_X100	700	/* ND 128            */
#define ND_STOPS_STEP_X100	10
#define ND_STOPS_DEF_X100	110

/* ── Media entity pad index ──────────────────────────────────── */
#define ND_PAD_SOURCE		0
#define ND_NUM_PADS		1

#endif /* _ND_CONTROLLER_H */
