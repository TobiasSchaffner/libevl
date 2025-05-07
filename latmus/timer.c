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
#include <sys/ioctl.h>
#include <evl/thread.h>
#include <evl/clock.h>
#include <evl/xbuf.h>
#include "timer.h"

static int lat_xfd = -1;

static struct latmus_measurement last_bulk;

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

void *timer_test_sitter(void *arg)
{
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
	mr.histogram_ptr = (__u64)(uintptr_t)histogram;
	mr.len = histogram ? histogram_cells * sizeof(int32_t) : 0;

	result.data_ptr = (__u64)(uintptr_t)&mr;
	result.len = sizeof(mr);

	notify_start();

	/* Run test until signal. */
	ret = oob_ioctl(latmus_fd, EVL_LATIOC_RUN, &result);
	if (ret)
		error(1, errno, "measurement failed");

	return NULL;
}

static void *xbuf_logger_thread(void *arg)
{
	struct latmus_measurement meas;
	ssize_t ret, round = 0;

	for (;;) {
		ret = read(lat_xfd, &meas, sizeof(meas));
		if (ret != sizeof(meas))
			break;
		log_results(&meas, round++);
	}

	/* Nobody waits for logger_done in timer mode. */

	return NULL;
}

void setup_measurement_on_timer(void)
{
	struct latmus_setup setup;
	pthread_attr_t attr;
	pthread_t sitter;
	int ret, sig;

	lat_xfd = evl_create_xbuf(1024, 0, 0, "lat-data:%d", getpid());
	if (lat_xfd < 0)
		error(1, -lat_xfd, "cannot create xbuf");

	create_logger(&logger, xbuf_logger_thread, NULL);

	memset(&setup, 0, sizeof(setup));
	setup.type = context_type;
	setup.period = period_usecs * 1000ULL; /* ns */
	setup.priority = responder_priority;
	setup.cpu = responder_cpu;
	setup.u.measure.xfd = lat_xfd;
	setup.u.measure.hcells = histogram ? histogram_cells : 0;
	ret = ioctl(latmus_fd, EVL_LATIOC_MEASURE, &setup);
	if (ret)
		error(1, errno, "measurement setup failed");

	if (context_type == EVL_LAT_USER)
		create_responder(&responder, responder_priority, timer_responder);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	ret = pthread_create(&sitter, &attr, timer_test_sitter, NULL);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "timer_test_sitter");

	sigwait(&sigmask, &sig);
	pthread_cancel(sitter);
	pthread_join(sitter, NULL);

	/*
	 * Add results from the last incomplete bulk once the sitter
	 * has returned to user-space from oob_ioctl(EVL_LATIOC_RUN)
	 * then exited, at which point such bulk contains meaningful
	 * data.
	 */
	if (last_bulk.samples > 0)
		__log_results(&last_bulk);
}

