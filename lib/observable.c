/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <evl/compiler.h>
#include <evl/sys.h>
#include <evl/thread.h>
#include <evl/observable.h>
#include <evl/syscall.h>
#include "internal.h"

int evl_create_observable(int flags, const char *fmt, ...)
{
	char *name = NULL;
	int ret, efd;
	va_list ap;

	if (fmt) {
		va_start(ap, fmt);
		ret = vasprintf(&name, fmt, ap);
		va_end(ap);
		if (ret < 0)
			return -ENOMEM;
	}

	efd = evl_create_element(EVL_OBSERVABLE_DEV, name, NULL, flags, NULL);
	if (name)
		free(name);

	return efd;
}

static bool wants_oob_io(void)
{
	/*
	 * Only non-EVL threads or members of the SCHED_WEAK class
	 * should call in-band.
	 */
	if (__evl_current == EVL_NO_HANDLE)
		return false;

	return !(evli_current_mode() & EVL_T_WEAK);
}

int evl_update_observable(int ofd, const struct evl_notice *ntc, int nr)
{
	ssize_t ret;

	if (!wants_oob_io())
		ret = write(ofd, ntc, nr * sizeof(*ntc));
	else
		ret = oob_write(ofd, ntc, nr * sizeof(*ntc));

	if (ret < 0)
		return errno == EAGAIN ? 0 : -errno;

	return ret / sizeof(*ntc);
}

static ssize_t do_read(int ofd, struct evl_notification *nf, int nr,
		ssize_t (*readfn)(int ofd, void *buf, size_t count))
{
	struct __evl_notification _nf __maybe_unused;
	ssize_t ret, _ret __maybe_unused;

	/*
	 * We can map the struct __evl_notification kernel type to
	 * struct evl_notification directly on 64bit platforms,
	 * because sizeof .tv_nsec is same size. We need to do some
	 * manual conversion on 32bit platforms though.
	 */
#if __WORDSIZE == 64
	ret = readfn(ofd, nf, nr * sizeof(*nf));
#else
	ret = 0;
	while (nr-- > 0) {
		_ret = readfn(ofd, &_nf, sizeof(_nf));
		if (_ret <= 0)
			return ret ?: _ret;
		nf->tag = _nf.tag;
		nf->serial = _nf.serial;
		nf->issuer = _nf.issuer;
		nf->event = _nf.event;
		nf->date.tv_sec = (long)_nf.date.tv_sec;
		nf->date.tv_nsec = _nf.date.tv_nsec;
		ret += sizeof(*nf);
		nf++;
	}
#endif

	return ret;
}

int evl_read_observable(int ofd, struct evl_notification *nf, int nr)
{
	ssize_t ret;

	if (!wants_oob_io())
		ret = do_read(ofd, nf, nr, read);
	else
		ret = do_read(ofd, nf, nr, oob_read);
	if (ret < 0)
		return -errno;

	return ret / sizeof(*nf);
}
