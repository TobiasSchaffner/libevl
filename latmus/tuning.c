/*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <error.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <evl/syscall.h>
#include <evl/devices/latmus.h>
#include "tuning.h"

void do_tuning(int type)
{
	struct latmus_result result;
	struct latmus_setup setup;
	pthread_t responder;
	__s32 gravity;
	int ret;

	if (verbosity) {
		printf("%s gravity...", context_labels[type]);
		fflush(stdout);
	}

	memset(&setup, 0, sizeof(setup));
	setup.type = type;
	setup.period = period_usecs * 1000ULL; /* ns */
	setup.priority = responder_priority;
	setup.cpu = responder_cpu;
	setup.u.tune.verbosity = verbosity;
	ret = ioctl(latmus_fd, EVL_LATIOC_TUNE, &setup);
	if (ret)
		error(1, errno, "tuning setup failed (%s)", context_labels[type]);

	if (type == EVL_LAT_USER)
		create_responder(&responder, responder_priority, timer_responder);

	pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL);

	notify_start();

	result.data_ptr = (__u64)(uintptr_t)&gravity;
	result.len = sizeof(gravity);
	ret = oob_ioctl(latmus_fd, EVL_LATIOC_RUN, &result);
	if (ret)
		error(1, errno, "measurement failed");

	if (type == EVL_LAT_USER)
		pthread_cancel(responder);

	if (verbosity)
		printf("%u ns\n", gravity);
}
