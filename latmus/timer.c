 /*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <error.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <evl/thread.h>
#include <evl/clock.h>
#include <evl/xbuf.h>
#include "latmus.h"
#include "stats.h"
#include "timer.h"

static int lat_xfd = -1;

static struct latmus_measurement last_bulk;

static pthread_t logger;

void *timer_responder(void *arg)
{
	__u64 timestamp = 0;
	struct timespec now;
	int ret, efd;

	/* Make it a public thread only for demo purpose. */
	efd = evl_attach_self("/timer-responder:%d", getpid());
	if (efd < 0)
		error(1, -efd, "evl_attach_self() failed");

	ret = evl_set_thread_mode(efd, EVL_T_WOSS, NULL);
	if (ret)
		error(1, -ret, "evl_set_thread_mode(EVL_T_WOSS) failed");

	for (;;) {
		ret = oob_ioctl(latmus_fd, EVL_LATIOC_PULSE, &timestamp);
		if (ret) {
			if (errno != EPIPE)
				error(1, errno, "pulse failed");
			timestamp = 0; /* Next period. */
		} else {
			evl_read_clock(EVL_CLOCK_MONOTONIC, &now);
			timestamp = (__u64)now.tv_sec * 1000000000 + now.tv_nsec;
		}
	}

	return NULL;
}

static void *timer_test_sitter(void *arg)
{
	struct statistics *st = arg;
	struct latmus_measurement_result mr;
	struct latmus_result result;
	int ret;

	/*
	 * Keep this service thread private by omitting the initial
	 * slash character in the name.
	 */
	ret = evl_attach_self("test-sitter:%d", getpid());
	if (ret < 0)
		error(1, -ret, "evl_attach_self() failed");

	mr.last_ptr = (__u64)(uintptr_t)&last_bulk;
	/*
	 * Tell the latmus driver where to dump the histogram data
	 * which it collects for us (we never call
	 * add_measurement_histogram() in timer/gpio test mode, unlike
	 * the net test which does).
	 */
	mr.histogram_ptr = (__u64)(uintptr_t)st->histogram;
	mr.len = st->h_cells * sizeof(int32_t);
	result.data_ptr = (__u64)(uintptr_t)&mr;
	result.len = sizeof(mr);

	notify_start(1); /* +1 warm-up time */

	/* Run test until signal. */
	ret = oob_ioctl(latmus_fd, EVL_LATIOC_RUN, &result);
	if (ret)
		error(1, errno, "measurement failed");

	return NULL;
}

static void *xbuf_logger_thread(void *arg)
{
	struct statistics *st = arg;
	struct latmus_measurement meas;
	ssize_t ret, round = 0;

	for (;;) {
		ret = read(lat_xfd, &meas, sizeof(meas));
		if (ret != sizeof(meas))
			break;
		log_results(st, &meas, round++);
	}

	return NULL;
}

void run_timer_test(size_t histogram_cells)
{
	struct statistics statistics = {
		.ops = {
			.more_data = more_timer_data,
			.wrap_data_page = wrap_timer_data_page,
			.print_summary = print_timer_summary,
			.get_elapsed_secs = get_timer_elapsed_secs,
		},
	};
	pthread_t responder, sitter;
	struct latmus_setup setup;
	pthread_attr_t attr;
	time_t duration;
	int ret, sig;

	lat_xfd = evl_create_xbuf(1024, 0, 0, "lat-data:%d", getpid());
	if (lat_xfd < 0)
		error(1, -lat_xfd, "cannot create xbuf");

	init_statistics("timer", &statistics, histogram_cells);
	create_logger(&logger, xbuf_logger_thread, &statistics);

	memset(&setup, 0, sizeof(setup));
	setup.type = context_type;
	setup.period = period_usecs * 1000ULL; /* ns */
	setup.priority = responder_priority;
	setup.cpu = responder_cpu;
	setup.u.measure.xfd = lat_xfd;
	setup.u.measure.hcells = statistics.h_cells;
	ret = ioctl(latmus_fd, EVL_LATIOC_MEASURE, &setup);
	if (ret)
		error(1, errno, "measurement setup failed");

	if (test_ulat)
		create_responder(&responder, responder_priority, timer_responder);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	ret = pthread_create(&sitter, &attr, timer_test_sitter, &statistics);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "timer_test_sitter");

	sigwait(&sigmask, &sig);
	pthread_cancel(sitter);
	pthread_join(sitter, NULL);
	pthread_cancel(responder);
	pthread_join(responder, NULL);
	pthread_cancel(logger);
	pthread_join(logger, NULL);

	/*
	 * Add results from the last incomplete bulk once the sitter
	 * has returned to user-space from oob_ioctl(EVL_LATIOC_RUN)
	 * then exited, at which point such bulk contains meaningful
	 * data.
	 */
	if (last_bulk.samples > 0)
		__log_results(&statistics, &last_bulk);

	duration = time(NULL) - start_time - 1; /* -1s warm-up time */
	consume_statistics(&statistics, 1, duration, spurious_inband_switches > 0);
}

int more_timer_data(struct statistics *st,
		const struct latmus_measurement *meas)
{
	double min, avg, max, best, worst;
	int ret = 0;

	min = (double)meas->min_lat / 1000.0;
	avg = (double)(meas->sum_lat / (int)meas->samples) / 1000.0;
	max = (double)meas->max_lat / 1000.0;
	best = (double)st->all_minlat / 1000.0;
	worst = (double)st->all_maxlat / 1000.0;

	/*
	 * A trivial check on the reported values, so that we detect
	 * and stop on obviously inconsistent results.
	 */
	if (min > max || min > avg || avg > max ||
		min > worst || max > worst || avg > worst ||
		best > worst || worst < best) {
		ret = -EINVAL;
		verbosity = 1;
	}

	if (verbosity > 0)
		printf("RTD|%11.3f|%11.3f|%11.3f|%8d|%6u|%11.3f|%11.3f\n",
			min, avg, max,
			st->all_overruns, spurious_inband_switches,
			best, worst);
	return ret;
}

void wrap_timer_data_page(struct statistics *st, unsigned int round)
{
	time_t now, dt;

	time(&now);
	dt = now - start_time - 1; /* -1s warm-up time */
	printf("RTT|  %.2ld:%.2ld:%.2ld  (%s, %u us period,",
		(long)(dt / 3600), (long)((dt / 60) % 60), (long)(dt % 60),
		context_labels[context_type], period_usecs);
	if (responder_priority != -1)
		printf(" priority %d,", responder_priority);
	printf(" CPU%d%s)\n",
		responder_cpu,
		responder_cpu_state & EVL_CPU_ISOL ? "" : "-noisol");
	printf("RTH|%11s|%11s|%11s|%8s|%6s|%11s|%11s\n",
		"----lat min", "----lat avg",
		"----lat max", "-overrun", "---msw",
		"---lat best", "--lat worst");
}

void print_timer_summary(struct statistics *st, time_t duration)
{
	time_t t = timeout ?: duration;

	if (st->all_samples == 0)
		return;

	printf("---|-----------|-----------|-----------|--------"
		"|------|-----------------------\n"
		"RTS|%11.3f|%11.3f|%11.3f|%8d|%6u|      "
		"%.2ld:%.2ld:%.2ld/%.2ld:%.2ld:%.2ld\n",
		(double)st->all_minlat / 1000.0,
		(double)(st->all_sum / st->all_samples) / 1000.0,
		(double)st->all_maxlat / 1000.0,
		st->all_overruns, spurious_inband_switches,
		(long)(duration / 3600), (long)((duration / 60) % 60),
		(long)(duration % 60), (long)(duration / 3600),
		(long)((t / 60) % 60), (long)(t % 60));
}

time_t get_timer_elapsed_secs(struct statistics *st)
{
	return time(NULL) - start_time - 1;
}
