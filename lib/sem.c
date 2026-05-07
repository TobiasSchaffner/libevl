/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
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

	evli_init_sem(&sem->sem, eids.sstate_offset);
	sem->u.active.efd = efd;
	sem->magic = __SEM_ACTIVE_MAGIC;

	return efd;
}

int evl_open_sem(struct evl_sem *sem, const char *fmt, ...)
{
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

	evli_init_sem(&sem->sem, bind.eids.sstate_offset);
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

	sem->u.active.efd = -1;
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

int evl_timedget_sem(struct evl_sem *sem, const struct timespec *timeout)
{
	struct evl_monitor_waitreq req;
	int ret, mode;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	/*
	 * Threads running in-band must take the slow path in order to
	 * switch oob prior to decrementing the semaphore, except
	 * weakly scheduled ones for which in-band is the nominal
	 * mode.
	 */
	mode = evli_current_mode();
	if ((mode & (EVL_T_INBAND|EVL_T_WEAK)) == EVL_T_INBAND)
		goto slow_path;

	ret = evli_tryget_sem(&sem->sem);
	if (ret != -EAGAIN)
		return ret;

slow_path:
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
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	return evli_tryget_sem(&sem->sem);
}

int evl_put_sem(struct evl_sem *sem)
{
	__s32 sigval;
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	ret = evli_tryput_sem(&sem->sem, &sigval);
	if (ret != -ENODATA)
		return ret;

	return __evl_transparent_call(sem->u.active.efd, ioctl,
				EVL_MONIOC_SIGNAL, &sigval);
}

int evl_flush_sem(struct evl_sem *sem)
{
	__s32 sigval = 1;
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	return __evl_transparent_call(sem->u.active.efd, ioctl,
				EVL_MONIOC_BROADCAST, &sigval);
}

int evl_peek_sem(struct evl_sem *sem, int *r_val)
{
	int ret;

	ret = check_sanity(sem);
	if (ret)
		return ret;

	*r_val = (int)evli_monitor_value(&sem->sem);

	return 0;
}
