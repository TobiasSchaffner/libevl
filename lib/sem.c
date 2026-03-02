/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdbool.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <evl/compiler.h>
#include <evl/atomic.h>
#include <evl/sys.h>
#include <evl/sem.h>
#include <evl/thread.h>
#include <evl/syscall.h>
#include "internal.h"

#define __SEM_ACTIVE_MAGIC	0xcb13cb13
#define __SEM_DEAD_MAGIC	0

int evl_create_sem(struct evl_sem *sem, int clockfd,
		int initval, int flags,
		const char *fmt, ...)
{
	struct __evl_monitor_sstate *state;
	struct evl_monitor_attrs attrs;
	struct evl_element_ids eids;
	char *name = NULL;
	int efd, ret;
	va_list ap;

	if (__evl_shared_memory == NULL)
		return -ENXIO;

	if (fmt) {
		va_start(ap, fmt);
		ret = vasprintf(&name, fmt, ap);
		va_end(ap);
		if (ret < 0)
			return -ENOMEM;
	}

	attrs.type = EVL_MONITOR_EVENT;
	attrs.protocol = EVL_EVENT_COUNT;
	attrs.clockfd = clockfd;
	attrs.initval = initval;
	efd = evl_create_element(EVL_MONITOR_DEV, name, &attrs,	flags, &eids);
	if (name)
		free(name);
	if (efd < 0)
		return efd;

	state = __evl_shared_memory + eids.sstate_offset;
	atomic_store(&state->u.event.value, initval);
	sem->u.active.sstate_offset = eids.sstate_offset;
	sem->u.active.fundle = eids.fundle;
	sem->u.active.efd = efd;
	sem->magic = __SEM_ACTIVE_MAGIC;

	return efd;
}

int evl_open_sem(struct evl_sem *sem, const char *fmt, ...)
{
	struct __evl_monitor_sstate *state;
	struct evl_monitor_binding bind;
	int ret, efd;
	va_list ap;

	if (__evl_shared_memory == NULL)
		return -ENXIO;

	va_start(ap, fmt);
	efd = evl_open_element_vargs(EVL_MONITOR_DEV, fmt, ap);
	va_end(ap);
	if (efd < 0)
		return efd;

	ret = ioctl(efd, EVL_MONIOC_BIND, &bind);
	if (ret) {
		ret = -errno;
		goto fail;
	}

	if (bind.type != EVL_MONITOR_EVENT ||
		bind.protocol != EVL_EVENT_COUNT) {
		ret = -EINVAL;
		goto fail;
	}

	state = __evl_shared_memory + bind.eids.sstate_offset;
	__force_pte_fixup(state->u.event.value);
	sem->u.active.sstate_offset = bind.eids.sstate_offset;
	sem->u.active.fundle = bind.eids.fundle;
	sem->u.active.efd = efd;
	sem->magic = __SEM_ACTIVE_MAGIC;

	return efd;
fail:
	close(efd);

	return ret;
}

int evl_close_sem(struct evl_sem *sem)
{
	int ret;

	if (sem->magic == __SEM_UNINIT_MAGIC)
		return 0;

	if (sem->magic != __SEM_ACTIVE_MAGIC)
		return -EINVAL;

	ret = close(sem->u.active.efd);
	if (ret)
		return -errno;

	sem->u.active.fundle = EVL_NO_HANDLE;
	sem->u.active.sstate_offset = ~0;
	sem->magic = __SEM_DEAD_MAGIC;

	return 0;
}

static int check_sanity(struct evl_sem *sem)
{
	int efd;

	if (sem->magic == __SEM_UNINIT_MAGIC) {
		efd = evl_create_sem(sem,
				sem->u.uninit.clockfd,
				sem->u.uninit.initval,
				sem->u.uninit.flags,
				"%s", sem->u.uninit.name);
		return efd < 0 ? efd : 0;
	}

	return sem->magic != __SEM_ACTIVE_MAGIC ? -EINVAL : 0;
}

static int try_get(struct __evl_monitor_sstate *state)
{
	__s32 val;

	val = atomic_load_explicit(&state->u.event.value, __ATOMIC_ACQUIRE);
	do {
		if (val <= 0)
			return -EAGAIN;
	} while (!atomic_compare_exchange_weak_explicit(
			&state->u.event.value, &val, val - 1,
			__ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

	return 0;
}

int evl_timedget_sem(struct evl_sem *sem, const struct timespec *timeout)
{
	struct __evl_monitor_sstate *state;
	struct evl_monitor_waitreq req;
	fundle_t current;
	int mode, ret;

	current = __evl_get_current();
	if (current == EVL_NO_HANDLE)
		return -EPERM;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	/*
	 * Threads running in-band must take the slow path in order to
	 * switch oob prior to decrementing the semaphore, except
	 * weakly scheduled ones for which in-band is the nominal
	 * mode.
	 */
	mode = __evl_get_current_mode();
	if (!(mode & EVL_T_INBAND) || (mode & EVL_T_WEAK)) {
		state = __evl_shared_memory + sem->u.active.sstate_offset;
		ret = try_get(state);
		if (ret != -EAGAIN)
			return ret;
	}

	req.gatefun = EVL_NO_HANDLE;
	req.timeout_ptr = __evl_ktimespec_ptr64(timeout);
	req.value = 0;		/* dummy */

	ret = oob_ioctl(sem->u.active.efd, EVL_MONIOC_WAIT, &req);

	return ret ? -errno : 0;
}

int evl_get_sem(struct evl_sem *sem)
{
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 0 };

	return evl_timedget_sem(sem, &timeout);
}

int evl_tryget_sem(struct evl_sem *sem)
{
	struct __evl_monitor_sstate *state;
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	state = __evl_shared_memory + sem->u.active.sstate_offset;

	return try_get(state);
}

static inline bool is_polled(struct __evl_monitor_sstate *state)
{
	return !!atomic_load(&state->u.event.pollrefs);
}

int evl_put_sem(struct evl_sem *sem)
{
	struct __evl_monitor_sstate *state;
	__s32 sigval = 1, val;
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	state = __evl_shared_memory + sem->u.active.sstate_offset;
	val = atomic_load_explicit(&state->u.event.value, __ATOMIC_ACQUIRE);
	if (val < 0 || is_polled(state)) {
	slow_path:
		if (__evl_get_current() && !__evl_is_inband())
			ret = oob_ioctl(sem->u.active.efd,
					EVL_MONIOC_SIGNAL, &sigval);
		else
			/* In-band threads may post pended sema4s. */
			ret = ioctl(sem->u.active.efd,
				EVL_MONIOC_SIGNAL, &sigval);
		return ret ? -errno : 0;
	}

	while (!atomic_compare_exchange_weak_explicit(
			&state->u.event.value, &val, val + 1,
			__ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
		if (val < 0)
			goto slow_path;
	}

	if (is_polled(state)) {
		sigval = 0;
		goto slow_path;
	}

	return 0;
}

int evl_flush_sem(struct evl_sem *sem)
{
	__s32 sigval = 1;
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	if (__evl_get_current() && !__evl_is_inband())
		ret = oob_ioctl(sem->u.active.efd, EVL_MONIOC_BROADCAST, &sigval);
	else
		ret = ioctl(sem->u.active.efd, EVL_MONIOC_BROADCAST, &sigval);

	return ret ? -errno : 0;
}

int evl_peek_sem(struct evl_sem *sem, int *r_val)
{
	struct __evl_monitor_sstate *state;

	if (sem->magic != __SEM_ACTIVE_MAGIC)
		return -EINVAL;

	state = __evl_shared_memory + sem->u.active.sstate_offset;
	*r_val = atomic_load(&state->u.event.value);

	return 0;
}
