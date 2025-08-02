/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2020 Philippe Gerum <rpm@xenomai.org>
 *
 * This file defines the message types exchanged over UDP with the
 * latency monitor (aka 'latmon') running on the Zephyr side. This
 * monitor observes and records the response time of the latmus
 * program to GPIO events.
 *
 * See https://github.com/zephyrproject-rtos/zephyr/tree/main/subsys/net/lib/latmon.
 */

#ifndef _EVL_LATMUS_LATMON_H
#define _EVL_LATMUS_LATMON_H

#include <stdint.h>

struct latmon_net_request {
	uint32_t period_usecs;
	uint32_t histogram_cells;
} __attribute__((__packed__));

struct latmon_net_data {
	int32_t sum_lat_hi;
	int32_t sum_lat_lo;
	int32_t min_lat;
	int32_t max_lat;
	uint32_t overruns;
	uint32_t samples;
} __attribute__((__packed__));

#endif /* !_EVL_LATMUS_LATMON_H */
