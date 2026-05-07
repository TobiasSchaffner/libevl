/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2019 Philippe Gerum  <rpm@xenomai.org>
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <evl/sys.h>
#include <evl/syscall.h>
#include <evl/timer.h>
#include "internal.h"

int evl_new_timer(int clockfd)
{
	int ret, efd = -1;

	if (__evl_mono_clockfd < 0)
		return -ENXIO;

	if (clockfd == EVL_CLOCK_MONOTONIC)
		clockfd = __evl_mono_clockfd;
	else if (clockfd == EVL_CLOCK_REALTIME)
		clockfd = __evl_real_clockfd;

	ret = ioctl(clockfd, EVL_CLKIOC_NEW_TIMER, &efd);
	if (ret)
		return -errno;

	return efd;
}

int evl_set_timer(int efd,
		const struct itimerspec *value,
		struct itimerspec *ovalue)
{
	struct evl_timerfd_setreq sreq;

	sreq.value_ptr = __evl_kitimerspec_ptr64(value);
	sreq.ovalue_ptr = __evl_kitimerspec_ptr64(ovalue);

	return __evl_transparent_call(efd, ioctl, EVL_TFDIOC_SET, &sreq);
}

int evl_get_timer(int efd, struct itimerspec *value)
{
	return __evl_transparent_call(efd, ioctl, EVL_TFDIOC_GET, value);
}
