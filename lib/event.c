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
#include <evl/event.h>
#include <evl/syscall.h>
#include "internal.h"

#define __EVENT_ACTIVE_MAGIC	0xef55ef55
#define __EVENT_DEAD_MAGIC	0

static int init_event_vargs(struct evl_event *event,
			int clockfd, int flags,
			const char *fmt, va_list ap)
{
	struct evl_monitor_attrs attrs;
	struct evl_element_ids eids;
	char *name = NULL;
	int efd, ret;

	if (__evl_shared_memory == NULL)
		return -ENXIO;

	if (fmt) {
		ret = vasprintf(&name, fmt, ap);
		if (ret < 0)
			return -ENOMEM;
	}

	attrs.type = EVL_MONITOR_EVENT;
	attrs.protocol = EVL_EVENT_GATED;
	attrs.clockfd = clockfd;
	attrs.initval = 0;
	efd = evl_create_element(EVL_MONITOR_DEV, name, &attrs, flags, &eids);
	if (name)
		free(name);
	if (efd < 0)
		return efd;

	evli_init_event(&event->event, eids.sstate_offset);
	event->u.active.efd = efd;
	event->magic = __EVENT_ACTIVE_MAGIC;

	return efd;
}

static int init_event_static(struct evl_event *event,
			int clockfd, int flags,
			const char *fmt, ...)
{
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = init_event_vargs(event, clockfd, flags, fmt, ap);
	va_end(ap);

	return efd;
}

static int open_event_vargs(struct evl_event *event,
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

	if (bind.type != EVL_MONITOR_EVENT || bind.protocol != EVL_EVENT_GATED) {
		ret = -EINVAL;
		goto fail;
	}

	evli_init_event(&event->event, bind.eids.sstate_offset);
	event->u.active.efd = efd;
	event->magic = __EVENT_ACTIVE_MAGIC;

	return 0;
fail:
	close(efd);

	return ret;
}

int evl_create_event(struct evl_event *event,
		int clockfd, int flags, const char *fmt, ...)
{
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = init_event_vargs(event, clockfd, flags, fmt, ap);
	va_end(ap);

	return efd;
}

int evl_open_event(struct evl_event *event, const char *fmt, ...)
{
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = open_event_vargs(event, fmt, ap);
	va_end(ap);

	return efd;
}

int evl_close_event(struct evl_event *event)
{
	int ret;

	if (event->magic == __EVENT_UNINIT_MAGIC)
		return 0;

	if (event->magic != __EVENT_ACTIVE_MAGIC)
		return -EINVAL;

	ret = close(event->u.active.efd);
	if (ret)
		return -errno;

	event->magic = __EVENT_DEAD_MAGIC;
	event->u.active.efd = -1;

	return 0;
}

static int check_event_sanity(struct evl_event *event)
{
	int efd;

	if (event->magic == __EVENT_UNINIT_MAGIC) {
		efd = init_event_static(event, event->u.uninit.clockfd,
					event->u.uninit.flags,
					event->u.uninit.name);
		if (efd < 0)
			return efd;
	} else if (event->magic != __EVENT_ACTIVE_MAGIC)
		return -EINVAL;

	return 0;
}

int evl_signal_event(struct evl_event *event)
{
	int ret;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	evli_signal_event(&event->event);

	return 0;
}

int evl_signal_thread(struct evl_event *event, int thrfd)
{
	fundle_t fundle;
	int ret;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	ret = evli_signal_targeted(&event->event);
	if (ret == -ENODATA) {
		fundle = evli_monitor_fundle(&event->event);
		ret = oob_ioctl(thrfd, EVL_THRIOC_SIGNAL, &fundle) ? -errno : 0;
	}

	return ret;
}

int evl_broadcast_event(struct evl_event *event)
{
	int ret;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	evli_broadcast_event(&event->event);

	return 0;
}

struct unwait_data {
	struct evl_monitor_unwaitreq ureq;
	int efd;
};

static void unwait_event(void *data)
{
	struct unwait_data *unwait = data;
	int ret;

	do
		ret = oob_ioctl(unwait->efd, EVL_MONIOC_UNWAIT,	&unwait->ureq);
	while (ret && errno == EINTR);
}

int evl_timedwait_event(struct evl_event *event,
			struct evl_mutex *mutex,
			const struct timespec *timeout)
{
	struct evl_monitor_waitreq req;
	struct unwait_data unwait;
	int ret;

	if (mutex->magic != __MUTEX_ACTIVE_MAGIC)
		return -EINVAL;

	ret = check_event_sanity(event);
	if (ret)
		return ret;

	req.gatefun = evli_monitor_fundle(&mutex->gate);
	req.timeout_ptr = __evl_ktimespec_ptr64(timeout);
	unwait.ureq.gatefun = req.gatefun;
	unwait.efd = event->u.active.efd;

	req.value = 0;		/* Only to please valgrind. */
	pthread_cleanup_push(unwait_event, &unwait);
	ret = oob_ioctl(event->u.active.efd, EVL_MONIOC_WAIT, &req);
	pthread_cleanup_pop(0);

	if (!ret)
		return 0;

	/*
	 * If oob_ioctl() failed for any reason but EIDRM or EPERM,
	 * the event is still valid and we should be allowed to lock
	 * the mutex guarding it, so we must issue MONIOC_UNWAIT to
	 * grab the mutex back for recovery.
	 */
	ret = -errno;
	if (ret != -EIDRM && ret != -EPERM)
		unwait_event(&unwait);

	/*
	 * If oob_ioctl() failed with EINTR, we got forcibly unblocked
	 * for handling a signal or any other reason while waiting for
	 * the event (leaving it unguarded) or reacquiring the mutex,
	 * in which case we return success since spurious wake ups are
	 * deemed ok, hoping for the next call to go to
	 * completion. Any other error is reported verbatim.
	 */
	return ret == -EINTR ? 0 : ret;
}

int evl_wait_event(struct evl_event *event, struct evl_mutex *mutex)
{
	struct timespec timeout = { .tv_sec = 0, .tv_nsec = 0 };

	return evl_timedwait_event(event, mutex, &timeout);
}
