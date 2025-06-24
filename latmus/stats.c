/*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <error.h>
#include <stdio.h>
#include <time.h>
#include <memory.h>
#include <stdlib.h>
#include <unistd.h>
#include <evl/evl.h>
#include "latmus.h"
#include "stats.h"

void init_statistics(const char *name,
		struct statistics *st, size_t h_cells)
{
	st->all_minlat = TEN_MILLIONS;
	st->all_maxlat = -TEN_MILLIONS;
	st->all_sum = 0;
	st->all_samples = 0;
	st->all_overruns = 0;
	st->peak_time = 0;
	st->h_cells = h_cells;
	st->histogram = NULL;
	st->name = name;

	if (h_cells) {
		st->histogram = malloc(h_cells * sizeof(int32_t));
		if (!st->histogram)
			error(1, ENOMEM,
				"cannot get memory for %s statistics", name);
		memset(st->histogram, 0, h_cells * sizeof(int32_t));
	}
}

void destroy_statistics(struct statistics *st)
{
	if (st->histogram)
		free(st->histogram);
}

void add_measurement(struct latmus_measurement *meas, __s64 dt)
{
	meas->sum_lat += dt;
	if (dt < meas->min_lat)
		meas->min_lat = (__s32)dt;
	if (dt > meas->max_lat)
		meas->max_lat = (__s32)dt;
	meas->samples++;
}

void add_measurement_histogram(struct statistics *st,
			struct latmus_measurement *meas, __s64 dt)
{
	add_measurement(meas, dt);

	if (st->histogram) {
		size_t cell = (dt < 0 ? -dt : dt) / 1000; /* us */
		if (cell >= st->h_cells)
			cell = st->h_cells - 1;
		st->histogram[cell]++;
	}
}

void reset_measurement(struct latmus_measurement *meas)
{
	meas->sum_lat = 0;
	meas->min_lat = TEN_MILLIONS;
	meas->max_lat = -TEN_MILLIONS;
	meas->overruns = 0;
	meas->samples = 0;
}

void __log_results(struct statistics *st,
		const struct latmus_measurement *meas)
{
	if (meas->min_lat < st->all_minlat)
		st->all_minlat = meas->min_lat;

	if (meas->max_lat > st->all_maxlat) {
		st->peak_time = st->ops.get_elapsed_secs(st);
		st->all_maxlat = meas->max_lat;
		if (abort_threshold && st->all_maxlat > abort_threshold) {
			fprintf(stderr, "latency threshold is exceeded"
				" (%d >= %.3f), aborting.\n",
				abort_threshold,
				(double)st->all_maxlat / 1000.0);
			exit(102);
		}
	}

	st->all_sum += meas->sum_lat;
	st->all_samples += meas->samples;
	st->all_overruns += meas->overruns;
}

void log_results(struct statistics *st,
		const struct latmus_measurement *meas,
		unsigned int round)
{
	int ret;

	if (verbosity > 0 && data_lines && (round % data_lines) == 0)
		st->ops.wrap_data_page(st, round);

	__log_results(st, meas);

	ret = st->ops.more_data(st, meas);
	if (ret) {
		fprintf(stderr, "results look weird, aborting.\n");
		exit(103);
	}
}

static void paste_file_in(const char *path, const char *header)
{
	char buf[BUFSIZ];
	FILE *fp;

	fp = fopen(path, "r");
	if (fp == NULL)
		return;

	fprintf(plot_fp, "# %s", header ?: "");

	while (fgets(buf, sizeof(buf), fp))
		fputs(buf, plot_fp);

	fclose(fp);
}

static void dump_gnuplot(struct statistics *st_array, int nr,
			time_t duration, bool degraded_mode)
{
	struct statistics *st;
	int first, last, n;
	bool outliers;

	fprintf(plot_fp, "# Test started on: %s", ctime(&start_time));
	if (degraded_mode)
		fprintf(plot_fp, "# DEGRADED MODE DETECTED - FIGURES MAY BE IRRELEVANT\n");
	paste_file_in("/proc/version", NULL);
	paste_file_in("/proc/cmdline", NULL);
	fprintf(plot_fp, "# libevl version: %s\n", evl_get_version().version_string);
	fprintf(plot_fp, "# sampling period: %u microseconds\n", period_usecs);
	paste_file_in("/sys/devices/virtual/clock/monotonic/gravity",
		"clock gravity: ");
	paste_file_in("/sys/devices/system/clocksource/clocksource0/current_clocksource",
		"clocksource: ");
	paste_file_in("/sys/devices/system/clocksource/clocksource0/vdso_clocksource",
		"vDSO access: ");
	fprintf(plot_fp, "# context: %s\n", context_labels[context_type]);
	if (responder_priority != -1) {
		fprintf(plot_fp, "# thread priority: %d\n", responder_priority);
		fprintf(plot_fp, "# thread affinity: CPU%d%s\n",
			responder_cpu,
			responder_cpu_state & EVL_CPU_ISOL ? "" : "-noisol");
	}
	if (c_state_restricted)
		fprintf(plot_fp, "# C-state restricted\n");
	fprintf(plot_fp, "# duration (hhmmss): %.2ld:%.2ld:%.2ld\n",
		(long)(duration / 3600), (long)((duration / 60) % 60), (long)(duration % 60));
	if (spurious_inband_switches > 0)
		fprintf(plot_fp, "# IN-BAND SWITCHES: %u\n", spurious_inband_switches);

	for (st = st_array; st < st_array + nr; st++) {
		if (st->all_samples == 0)
			continue;
		if (st->all_overruns > 0)
			fprintf(plot_fp, "# %s OVERRUNS: %u\n",
				st->name, st->all_overruns);
		fprintf(plot_fp, "# %s peak (hhmmss): %.2ld:%.2ld:%.2ld\n",
			st->name, (long)(st->peak_time / 3600),
			(long)((st->peak_time / 60) % 60),
			(long)(st->peak_time % 60));
		fprintf(plot_fp, "# %s min latency: %.3f\n",
			st->name, (double)st->all_minlat / 1000.0);
		fprintf(plot_fp, "# %s avg latency: %.3f\n",
			st->name, (double)(st->all_sum / st->all_samples) / 1000.0);
		fprintf(plot_fp, "# %s max latency: %.3f\n",
			st->name, (double)st->all_maxlat / 1000.0);
		fprintf(plot_fp, "# %s sample count: %lld\n",
			st->name, (long long)st->all_samples);
	}

	/*
	 * Shrink the display window to eliminate zero
	 * heading/trailing series.  All stat bulks which we want to
	 * be merged for display have histograms of the same size.
	 */
	for (n = 0; (size_t)n < st_array->h_cells; n++)
		for (st = st_array; st < st_array + nr; st++)
			if (st->histogram[n])
				goto next;
	n = 0;
next:
	first = n;
	for (n = st_array->h_cells - 1; n >= 0; n--)
		for (st = st_array; st < st_array + nr; st++)
			if (st->histogram[n])
				goto done;
	n = 0;
done:
	last = n;
	for (n = first; n < last; n++) {
		fprintf(plot_fp, "%d", n);
		for (st = st_array; st < st_array + nr; st++)
			fprintf(plot_fp, " %d", st->histogram[n]);
		fputc('\n', plot_fp);
	}

	/*
	 * If we have outliers in any of the bulks, display a '+' sign
	 * after the last cell index.
	 */
	for (st = st_array, outliers = false; st < st_array + nr; st++) {
		if ((size_t)(st->all_maxlat / 1000) >= st->h_cells) {
			outliers = true;
			break;
		}
	}

	fprintf(plot_fp, "%d%s", last, outliers ? "+" : "");

	for (st = st_array; st < st_array + nr; st++)
		fprintf(plot_fp, " %d", st->histogram[last]);

	fputc('\n', plot_fp);
}

void consume_statistics(struct statistics *st_array, int nr,
			time_t duration, bool degraded_mode)
{
	int n;

	st_array->ops.print_summary(st_array, duration);

	if (plot_fp) {
		dump_gnuplot(st_array, nr, duration, degraded_mode);
		if (plot_fp != stdout)
			fclose(plot_fp);
	}

	for (n = 0; n < nr; n++)
		destroy_statistics(st_array + n);
}
