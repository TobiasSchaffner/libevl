/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <evl/sys.h>
#include <evl/mutex.h>
#include <evl/syscall.h>
#include "internal.h"

#define __MUTEX_DEAD_MAGIC	0

static int init_mutex_vargs(struct evl_mutex *mutex,
			int protocol, int clockfd,
			unsigned int ceiling, int flags,
			const char *fmt, va_list ap)
{
	struct evl_monitor_attrs attrs;
	struct evl_element_ids eids;
	char *name = NULL;
	int efd, ret;

	if (__evl_shared_memory == NULL)
		return -ENXIO;

	/*
	 * We align on the in-band SCHED_FIFO priority range. Although
	 * the core does not require this, a 1:1 mapping between
	 * in-band and out-of-band priorities is simpler to deal with
	 * for users with respect to inband <-> out-of-band mode
	 * switches.
	 */
	if (protocol == EVL_GATE_PP) {
		ret = sched_get_priority_max(SCHED_FIFO);
		if (ret < 0 || ceiling == 0 || ceiling > (unsigned int)ret)
			return -EINVAL;
	}

	if (fmt) {
		ret = vasprintf(&name, fmt, ap);
		if (ret < 0)
			return -ENOMEM;
	}

	attrs.type = EVL_MONITOR_GATE;
	attrs.protocol = protocol;
	attrs.clockfd = clockfd;
	attrs.ceiling = ceiling;
	attrs.recursive = !!(flags & EVL_MUTEX_RECURSIVE);
	efd = evl_create_element(EVL_MONITOR_DEV, name, &attrs,	flags, &eids);
	if (name)
		free(name);
	if (efd < 0)
		return efd;

	evli_init_gate(&mutex->gate, eids.sstate_offset);
	mutex->u.active.efd = efd;
	mutex->magic = __MUTEX_ACTIVE_MAGIC;

	return efd;
}

static int init_mutex_static(struct evl_mutex *mutex,
			int clockfd, unsigned int ceiling,
			int flags, const char *fmt, ...)
{
	int efd, protocol = ceiling ? EVL_GATE_PP : EVL_GATE_PI;
	va_list ap;

	va_start(ap, fmt);
	efd = init_mutex_vargs(mutex, protocol, clockfd,
			ceiling, flags, fmt, ap);
	va_end(ap);

	return efd;
}

static int open_mutex_vargs(struct evl_mutex *mutex,
			const char *fmt, va_list ap)
{
	struct evl_monitor_binding bind;
	int ret, efd;

	efd = evl_open_element_vargs(EVL_MONITOR_DEV, fmt, ap);
	if (efd < 0)
		return efd;

	ret = ioctl(efd, EVL_MONIOC_BIND, &bind);
	if (ret) {
		ret = -errno;
		goto fail;
	}

	if (bind.type != EVL_MONITOR_GATE) {
		ret = -EINVAL;
		goto fail;
	}

	evli_init_gate(&mutex->gate, bind.eids.sstate_offset);
	mutex->u.active.efd = efd;
	mutex->magic = __MUTEX_ACTIVE_MAGIC;

	return 0;
fail:
	close(efd);

	return ret;
}

int evl_create_mutex(struct evl_mutex *mutex,
		int clockfd, unsigned int ceiling,
		int flags, const char *fmt, ...)
{
	int efd, protocol;
	va_list ap;

	protocol = ceiling ? EVL_GATE_PP : EVL_GATE_PI;
	va_start(ap, fmt);
	efd = init_mutex_vargs(mutex, protocol,
			clockfd, ceiling, flags, fmt, ap);
	va_end(ap);

	return efd;
}

int evl_open_mutex(struct evl_mutex *mutex, const char *fmt, ...)
{
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = open_mutex_vargs(mutex, fmt, ap);
	va_end(ap);

	return efd;
}

int evl_close_mutex(struct evl_mutex *mutex)
{
	int ret;

	if (mutex->magic == __MUTEX_UNINIT_MAGIC)
		return 0;

	if (mutex->magic != __MUTEX_ACTIVE_MAGIC)
		return -EINVAL;

	ret = close(mutex->u.active.efd);
	if (ret)
		return -errno;

	mutex->magic = __MUTEX_DEAD_MAGIC;
	mutex->u.active.efd = -1;

	return 0;
}

static int try_lock(struct evl_mutex *mutex)
{
	int ret;

	if (mutex->magic == __MUTEX_UNINIT_MAGIC) {
		ret = init_mutex_static(mutex,
				mutex->u.uninit.clockfd,
				mutex->u.uninit.ceiling,
				mutex->u.uninit.flags,
				mutex->u.uninit.name);
		if (ret < 0)
			return ret;
	} else if (mutex->magic != __MUTEX_ACTIVE_MAGIC)
		return -EINVAL;

	return evli_trylock_gate(&mutex->gate);
}

int evl_timedlock_mutex(struct evl_mutex *mutex,
			const struct timespec *timeout)
{
	int ret;

	ret = try_lock(mutex);
	if (ret != -ENODATA)
		return ret;

	do
		ret = oob_ioctl(mutex->u.active.efd, EVL_MONIOC_ENTER, timeout);
	while (ret && errno == EINTR);

	return ret ? -errno : 0;
}

int evl_lock_mutex(struct evl_mutex *mutex)
{
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 0 };

	return evl_timedlock_mutex(mutex, &timeout);
}

int evl_trylock_mutex(struct evl_mutex *mutex)
{
	int ret;

	ret = try_lock(mutex);
	if (ret != -ENODATA)
		return ret;

	do
		ret = oob_ioctl(mutex->u.active.efd, EVL_MONIOC_TRYENTER);
	while (ret && errno == EINTR);

	return ret ? -errno : 0;
}

int evl_unlock_mutex(struct evl_mutex *mutex)
{
	int ret;

	if (mutex->magic != __MUTEX_ACTIVE_MAGIC)
		return -EINVAL;

	ret = evli_tryunlock_gate(&mutex->gate);
	if (ret != -ENODATA)
		return ret;

	ret = oob_ioctl(mutex->u.active.efd, EVL_MONIOC_EXIT);

	return ret ? -errno : 0;
}

int evl_set_mutex_ceiling(struct evl_mutex *mutex,
			unsigned int ceiling)
{
	if (mutex->magic == __MUTEX_UNINIT_MAGIC) {
		if (mutex->u.uninit.ceiling == 0)
			return -EINVAL;
		mutex->u.uninit.ceiling = ceiling;
		return 0;
	}

	if (mutex->magic != __MUTEX_ACTIVE_MAGIC)
		return -EINVAL;

	return evli_set_gate_ceiling(&mutex->gate, ceiling);
}

int evl_get_mutex_ceiling(struct evl_mutex *mutex)
{
	if (mutex->magic == __MUTEX_UNINIT_MAGIC)
		return mutex->u.uninit.ceiling;

	if (mutex->magic != __MUTEX_ACTIVE_MAGIC)
		return -EINVAL;

	return evli_get_gate_ceiling(&mutex->gate);
}
