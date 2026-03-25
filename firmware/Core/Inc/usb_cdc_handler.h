/* SPDX-License-Identifier: GPL-2.0-only
 * usb_cdc_handler.h — USB CDC packet dispatch
 */
#pragma once
#include <stdint.h>

#define USB_PKT_LEN     4

/* Commands (host → device) */
#define CMD_WRITE_TARGET    0x10
#define CMD_READ_ACTUAL     0x11
#define CMD_WRITE_MODE      0x12
#define CMD_READ_TEMP       0x13
#define CMD_READ_STATUS     0x14
#define CMD_READ_CCM_IDX    0x15
#define CMD_READ_WIPER      0x16
#define CMD_READ_DC_OFFSET  0x17
#define CMD_WRITE_CAL_TRIG  0x20
#define CMD_WRITE_CAL_NEXT  0x22
#define CMD_WRITE_CAL_LUT   0x23
#define CMD_READ_FW_VER     0x30
#define CMD_PING            0xFF

/* Called from usbd_cdc_if.c CDC_Receive_FS callback */
void usb_cdc_on_rx(const uint8_t *buf, uint32_t len);

/* FreeRTOS task — blocked on g_usb_rx_sem */
void usb_task(void *arg);
