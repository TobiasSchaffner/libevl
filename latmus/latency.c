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
#include <stdlib.h>
#include <unistd.h>
#include <evl/evl.h>
#include "timer.h"
#include "gpio.h"

cpu_set_t isolated_cpus;

sigset_t sigmask;

int verbosity = 1,
	abort_threshold = 0;

time_t timeout = 0;

bool abort_on_switch = true,
	c_state_restricted = false;

pthread_t responder = 0, logger = 0;

unsigned int all_overruns = 0, spurious_inband_switches = 0;

int32_t all_minlat = TEN_MILLIONS, all_maxlat = -TEN_MILLIONS, *histogram;

int64_t all_sum = 0, all_samples = 0;

time_t start_time = 0, peak_time = 0;

FILE *plot_fp = NULL;

int data_lines = 21;

size_t histogram_cells = 200;

int context_type = EVL_LAT_USER;

int responder_priority = 98;

int responder_cpu = -1;

int responder_cpu_state = 0;

unsigned int period_usecs = 1000; /* 1ms */

sem_t logger_done;

const char *context_labels[] = {
	[EVL_LAT_IRQ] = "irq",
	[EVL_LAT_SIRQ] = "sirq",
	[EVL_LAT_KERN] = "kernel",
	[EVL_LAT_USER] = "user",
	[EVL_LAT_OOB_GPIO] = "oob-gpio",
	[EVL_LAT_INBAND_GPIO] = "inband-gpio",
};

int latmus_fd = -1;		/* FIXME: move this to test impl */

void notify_start(void)
{
	if (timeout)
		alarm(timeout + 1); /* +1 warm-up time */
}

void create_responder(pthread_t *tid, int priority, void *(*responder)(void *))
{
	struct sched_param param;
	pthread_attr_t attr;
	int ret;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	param.sched_priority = priority;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	ret = pthread_create(tid, &attr, responder, NULL);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "sampling thread");
}

void create_logger(pthread_t *tid, void *(*logger)(void *), void *arg)
{
	struct sched_param param;
	pthread_attr_t attr;
	int ret;

	sem_init(&logger_done, 0, 0);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
	param.sched_priority = 0;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	ret = pthread_create(tid, &attr, logger, arg);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "logger thread");
}

void __log_results(struct latmus_measurement *meas)
{
	if (meas->min_lat < all_minlat)
		all_minlat = meas->min_lat;
	if (meas->max_lat > all_maxlat) {
		peak_time = time(NULL) - start_time - 1;
		all_maxlat = meas->max_lat;
		if (abort_threshold && all_maxlat > abort_threshold) {
			fprintf(stderr, "latency threshold is exceeded"
				" (%d >= %.3f), aborting.\n",
				abort_threshold,
				(double)all_maxlat / 1000.0);
			exit(102);
		}
	}

	all_sum += meas->sum_lat;
	all_samples += meas->samples;
	all_overruns += meas->overruns;
}

void log_results(struct latmus_measurement *meas,
		unsigned int round)
{
	double min, avg, max, best, worst;
	bool oops = false;
	time_t now, dt;

	if (verbosity > 0 && data_lines && (round % data_lines) == 0) {
		time(&now);
		dt = now - start_time - 1; /* -1s warm-up time */
		printf("RTT|  %.2ld:%.2ld:%.2ld  (%s, %u us period,",
			(long)(dt / 3600), (long)((dt / 60) % 60), (long)(dt % 60),
			context_labels[context_type], period_usecs);
		if (context_type != EVL_LAT_IRQ && context_type != EVL_LAT_SIRQ)
			printf(" priority %d,", responder_priority);
		printf(" CPU%d%s)\n",
			responder_cpu,
			responder_cpu_state & EVL_CPU_ISOL ? "" : "-noisol");
		printf("RTH|%11s|%11s|%11s|%8s|%6s|%11s|%11s\n",
		       "----lat min", "----lat avg",
		       "----lat max", "-overrun", "---msw",
		       "---lat best", "--lat worst");
	}

	__log_results(meas);
	min = (double)meas->min_lat / 1000.0;
	avg = (double)(meas->sum_lat / (int)meas->samples) / 1000.0;
	max = (double)meas->max_lat / 1000.0;
	best = (double)all_minlat / 1000.0;
	worst = (double)all_maxlat / 1000.0;

	/*
	 * A trivial check on the reported values, so that we detect
	 * and stop on obviously inconsistent results.
	 */
	if (min > max || min > avg || avg > max ||
		min > worst || max > worst || avg > worst ||
		best > worst || worst < best) {
		oops = true;
		verbosity = 1;
	}

	if (verbosity > 0)
		printf("RTD|%11.3f|%11.3f|%11.3f|%8d|%6u|%11.3f|%11.3f\n",
			min, avg, max,
			all_overruns, spurious_inband_switches,
			best, worst);

	if (oops) {
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

static void dump_gnuplot(time_t duration, bool threaded)
{
	int first, last, n;

	if (all_samples == 0)
		return;

	fprintf(plot_fp, "# test started on: %s", ctime(&start_time));
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
	if (threaded) {
		fprintf(plot_fp, "# thread priority: %d\n", responder_priority);
		fprintf(plot_fp, "# thread affinity: CPU%d%s\n",
			responder_cpu,
			responder_cpu_state & EVL_CPU_ISOL ? "" : "-noisol");
	}
	if (c_state_restricted)
		fprintf(plot_fp, "# C-state restricted\n");
	fprintf(plot_fp, "# duration (hhmmss): %.2ld:%.2ld:%.2ld\n",
		(long)(duration / 3600), (long)((duration / 60) % 60), (long)(duration % 60));
	fprintf(plot_fp, "# peak (hhmmss): %.2ld:%.2ld:%.2ld\n",
		(long)(peak_time / 3600), (long)((peak_time / 60) % 60), (long)(peak_time % 60));
	if (all_overruns > 0)
		fprintf(plot_fp, "# OVERRUNS: %u\n", all_overruns);
	if (spurious_inband_switches > 0)
		fprintf(plot_fp, "# IN-BAND SWITCHES: %u\n", spurious_inband_switches);
	fprintf(plot_fp, "# min latency: %.3f\n",
		(double)all_minlat / 1000.0);
	fprintf(plot_fp, "# avg latency: %.3f\n",
		(double)(all_sum / all_samples) / 1000.0);
	fprintf(plot_fp, "# max latency: %.3f\n",
		(double)all_maxlat / 1000.0);
	fprintf(plot_fp, "# sample count: %lld\n",
		(long long)all_samples);

	for (n = 0; (size_t)n < histogram_cells && histogram[n] == 0; n++)
		;
	first = n;

	for (n = histogram_cells - 1; n >= 0 && histogram[n] == 0; n--)
		;
	last = n;

	for (n = first; n < last; n++)
		fprintf(plot_fp, "%d %d\n", n, histogram[n]);

	/*
	 * If we have outliers, display a '+' sign after the last cell
	 * index.
	 */
	fprintf(plot_fp, "%d%s %d\n", last,
		(size_t)(all_maxlat / 1000) >= histogram_cells ? "+" : "",
		histogram[last]);
}

void do_measurement(int type, bool threaded, bool oob_mode)
{
	const char *cpu_s = "";
	time_t duration;

	context_type = type;

	if (plot_fp) {
		histogram = malloc(histogram_cells * sizeof(int32_t));
		if (histogram == NULL)
			error(1, ENOMEM, "cannot get memory");
	}

	if (!(responder_cpu_state & EVL_CPU_ISOL))
		cpu_s = " (not isolated)";

	if (verbosity > 0)
		fprintf(stderr, "warming up on CPU%d%s...\n", responder_cpu, cpu_s);
	else
		fprintf(stderr, "running quietly for %ld seconds on CPU%d%s\n",
			(long)timeout, responder_cpu, cpu_s);

	switch (type) {
	case EVL_LAT_OOB_GPIO:
	case EVL_LAT_INBAND_GPIO:
		setup_measurement_on_gpio(oob_mode);
		break;
	default:
		setup_measurement_on_timer();
	}

	duration = time(NULL) - start_time - 1; /* -1s warm-up time */
	if (plot_fp) {
		dump_gnuplot(duration, threaded);
		if (plot_fp != stdout)
			fclose(plot_fp);
		free(histogram);
	}

	if (!timeout)
		timeout = duration;

	if (all_samples > 0)
		printf("---|-----------|-----------|-----------|--------"
			"|------|-------------------------\n"
			"RTS|%11.3f|%11.3f|%11.3f|%8d|%6u|    "
			"%.2ld:%.2ld:%.2ld/%.2ld:%.2ld:%.2ld\n",
			(double)all_minlat / 1000.0,
			(double)(all_sum / all_samples) / 1000.0,
			(double)all_maxlat / 1000.0,
			all_overruns, spurious_inband_switches,
			(long)(duration / 3600), (long)((duration / 60) % 60),
			(long)(duration % 60), (long)(duration / 3600),
			(long)((timeout / 60) % 60), (long)(timeout % 60));

	if (spurious_inband_switches > 0) {
		if (all_samples > 0)
			fputc('\n', stderr);
		fprintf(stderr, "*** WARNING: unexpected switches to in-band mode detected,\n"
		       "             latency figures displayed are NOT reliable.\n"
		       "             Please submit a bug report upstream.\n");
		if (abort_on_switch) {
			abort_on_switch = false;
			fprintf(stderr, "*** OOPS: aborting upon spurious switch to in-band mode.\n");
		}
	}

	if (responder)
		pthread_cancel(responder);

	if (logger)
		pthread_cancel(logger);
}
