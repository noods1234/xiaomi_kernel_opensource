/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * cinema.h — shared API between cinema_mode.c and cinema_end.c
 *
 * Copyright (c) 2024, Xiaomi Cinema Kernel Project
 */
#ifndef _DRIVERS_MISC_CINEMA_H
#define _DRIVERS_MISC_CINEMA_H

#include <linux/types.h>

/**
 * cinema_mode_set_active - activate or deactivate cinema performance mode.
 * @on: true to activate (CPU floor, latency QoS, wakeup source);
 *      false to release all constraints.
 *
 * Exported by cinema_mode.c for use by cinema_end.c.
 * Must not be called with cinema_lock held.
 * Returns 0 on success, negative errno on failure.
 */
int cinema_mode_set_active(bool on);

#endif /* _DRIVERS_MISC_CINEMA_H */
