/*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_LATMUS_STATS_H
#define _EVL_LATMUS_STATS_H

#include <time.h>
#include <stdint.h>
#include <evl/devices/latmus-abi.h>

struct statistics {
	int32_t all_minlat;
	int32_t all_maxlat;
	int64_t all_sum;
	int64_t all_samples;
	unsigned int all_overruns;
	time_t peak_time;
	int32_t *histogram;
	size_t h_cells;
	const char *name;
	struct __net_stat_ops {
		int (*more_data)(struct statistics *st,
				const struct latmus_measurement *meas);
		void (*wrap_data_page)(struct statistics *st,
				unsigned int round);
		void (*print_summary)(struct statistics *st_array, time_t duration);
		time_t (*get_elapsed_secs)(struct statistics *st_array);
	} ops;
};

void init_statistics(const char *name,
		struct statistics *st, size_t h_cells);

void destroy_statistics(struct statistics *st);

void reset_measurement(struct latmus_measurement *meas);

void add_measurement(struct latmus_measurement *meas, __s64 t);

void add_measurement_histogram(struct statistics *st,
			struct latmus_measurement *meas, __s64 t);

void __log_results(struct statistics *st,
		const struct latmus_measurement *meas);

void log_results(struct statistics *st,
		const struct latmus_measurement *meas,
		unsigned int round);

void consume_statistics(struct statistics *st_array, int nr,
			time_t duration,
			bool degraded_mode);

#endif /* !_EVL_LATMUS_STATS_H */
