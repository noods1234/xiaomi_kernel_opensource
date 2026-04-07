/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cinema_protocol.h — Framed binary protocol between cinema_end_daemon
 *                     and the STM32U5 MCU over USB-CDC.
 *
 * Frame layout:
 *   ┌────────┬─────────┬────────┬──────────────────┬──────────┐
 *   │  0xCE  │  CMD    │  LEN   │    PAYLOAD        │  CRC8    │
 *   │ 1 byte │ 1 byte  │ 1 byte │   0–255 bytes     │  1 byte  │
 *   └────────┴─────────┴────────┴──────────────────┴──────────┘
 *
 * The magic byte 0xCE re-synchronises the receiver after a partial
 * read or single-byte corruption.  CRC8/SMBUS covers CMD + LEN + PAYLOAD.
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */

#ifndef CINEMA_PROTOCOL_H
#define CINEMA_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Frame constants                                                      */
/* ------------------------------------------------------------------ */

#define CINEMA_PROTO_MAGIC      0xCE

/* Frame field offsets */
#define FRAME_OFF_MAGIC         0
#define FRAME_OFF_CMD           1
#define FRAME_OFF_LEN           2
#define FRAME_OFF_PAYLOAD       3

/* Overhead: magic(1) + cmd(1) + len(1) + crc(1) */
#define FRAME_OVERHEAD          4
#define FRAME_MAX_PAYLOAD       255
#define FRAME_MAX_LEN           (FRAME_OVERHEAD + FRAME_MAX_PAYLOAD)

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

/*
 * Direction prefixes:
 *   D→M  daemon-to-MCU (host to device)
 *   M→D  MCU-to-daemon (device to host, asynchronous or response)
 */

#define CMD_SET_ND              0x01  /* D→M: set_nd_payload              */
#define CMD_QUERY               0x02  /* D→M: no payload, request telem   */
#define CMD_TELEMETRY           0x03  /* M→D: telem_payload               */
#define CMD_FAULT               0x04  /* M→D: uint8_t reason (async)      */
#define CMD_CAL_REQ             0x05  /* D→M: no payload                  */
#define CMD_CAL_DATA            0x06  /* M→D: cal_curve payload           */
#define CMD_ACK                 0x07  /* M→D: uint8_t acked_cmd           */
#define CMD_NACK                0x08  /* M→D: uint8_t failed_cmd, reason  */

/* ------------------------------------------------------------------ */
/* Fault reason codes (CMD_FAULT payload byte)                          */
/* ------------------------------------------------------------------ */

#define FAULT_OVER_TEMP         0x01  /* cell_temp exceeded hard limit    */
#define FAULT_DAC_FAIL          0x02  /* AD5696R I2C error                */
#define FAULT_SUPPLY_FAIL       0x03  /* TPS65131 PGOOD lost              */
#define FAULT_SETTLE_TIMEOUT    0x04  /* nd_actual failed to converge     */

/* ------------------------------------------------------------------ */
/* ND and temperature limits (must match cinema_end.c constants)        */
/* ------------------------------------------------------------------ */

#define ND_MIN_MB               0       /* 0 stops                        */
#define ND_MAX_MB               7000    /* 7 stops                        */
#define ND_SETTLE_TOLERANCE_MB  25      /* ±25 mB = settled               */
#define ND_SETTLE_TIMEOUT_MS    500     /* ms before settle warning        */

#define TEMP_WARN_MC            55000   /* 55°C soft warning              */
#define TEMP_FAULT_MC           65000   /* 65°C hard fault                */

/* ------------------------------------------------------------------ */
/* Payload structures (packed — wire format)                            */
/* ------------------------------------------------------------------ */

/* CMD_SET_ND payload (4 bytes) */
struct __attribute__((packed)) set_nd_payload {
    int32_t nd_mb;          /* target ND in millibels [0, 7000]           */
};

/* CMD_TELEMETRY payload (8 bytes) */
struct __attribute__((packed)) telem_payload {
    int32_t nd_mb;          /* actual ND in millibels                     */
    int32_t temp_mc;        /* cell temperature in m°C                    */
};

/* CMD_ACK / CMD_NACK payload (1–2 bytes) */
struct __attribute__((packed)) ack_payload {
    uint8_t acked_cmd;
};

struct __attribute__((packed)) nack_payload {
    uint8_t failed_cmd;
    uint8_t reason;
};

/* CMD_CAL_DATA: variable length array of calibration points */
struct __attribute__((packed)) cal_point {
    int32_t nd_mb;          /* ND value in millibels                      */
    uint16_t dac_counts;    /* AD5696R 16-bit DAC value                   */
    int16_t  temp_offset_mc;/* temperature at which this point was taken  */
};

/* ------------------------------------------------------------------ */
/* CRC-8/SMBUS (polynomial 0x07)                                        */
/* Covers CMD + LEN + PAYLOAD bytes (not magic, not the CRC itself).   */
/* ------------------------------------------------------------------ */

static inline uint8_t cinema_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Frame builder                                                         */
/* ------------------------------------------------------------------ */

/*
 * cinema_build_frame - serialise a command into buf.
 *
 * Returns total frame length (FRAME_OVERHEAD + payload_len).
 * buf must be at least FRAME_MAX_LEN bytes.
 */
static inline int cinema_build_frame(uint8_t *buf,
                                     uint8_t cmd,
                                     const void *payload,
                                     uint8_t payload_len)
{
    buf[FRAME_OFF_MAGIC]   = CINEMA_PROTO_MAGIC;
    buf[FRAME_OFF_CMD]     = cmd;
    buf[FRAME_OFF_LEN]     = payload_len;

    if (payload_len && payload)
        __builtin_memcpy(&buf[FRAME_OFF_PAYLOAD], payload, payload_len);

    /* CRC covers cmd + len + payload */
    buf[FRAME_OFF_PAYLOAD + payload_len] =
        cinema_crc8(&buf[FRAME_OFF_CMD], 2 + payload_len);

    return FRAME_OVERHEAD + payload_len;
}

#endif /* CINEMA_PROTOCOL_H */
