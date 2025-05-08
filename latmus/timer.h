/*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_LATMUS_TIMER_H
#define _EVL_LATMUS_TIMER_H

#include <stdbool.h>
#include <sys/types.h>

void run_timer_test(size_t histogram_cells);

void *timer_responder(void *arg);

void wrap_timer_data_page(struct statistics *st,
			unsigned int round);

int more_timer_data(struct statistics *st,
		const struct latmus_measurement *meas);

void print_timer_summary(struct statistics *st,
			time_t duration);

#endif /* !_EVL_LATMUS_TIMER_H */
