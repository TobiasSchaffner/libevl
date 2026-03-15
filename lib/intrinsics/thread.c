/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018, 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sched.h>
#include <stdlib.h>
#include <evl/compiler.h>
#include <evl/sys.h>
#include <evl/factory-abi.h>
#include <evl/intrinsics/thread.h>
#include "internal.h"

__thread __attribute__ ((tls_model (EVL_TLS_MODEL)))
fundle_t __evl_current = EVL_NO_HANDLE;

__thread __attribute__ ((tls_model (EVL_TLS_MODEL)))
struct __evl_thread_sstate *__evl_current_sstate;

void evli_clear_tls(void)
{
	__evl_current = EVL_NO_HANDLE;
	__evl_current_sstate = NULL;
}

int evli_attach_thread(struct evli_thread *thread,
		int flags, const char *fmt, va_list ap)
{
	struct evl_element_ids eids;
	char *name = NULL;
	int efd, ret;

	/*
	 * Cannot bind twice. Although the core would catch it, we can
	 * detect this issue early.
	 */
	if (__evl_current != EVL_NO_HANDLE)
		return -EBUSY;

	if (fmt) {
		ret = vasprintf(&name, fmt, ap);
		if (ret < 0)
			return -ENOMEM;
	}

	efd = evl_create_element(EVL_THREAD_DEV, name, NULL, flags, &eids);
	if (name)
		free(name);
	if (efd < 0)
		return efd;

	thread->sstate_offset = eids.sstate_offset;
	__evl_current_sstate = __evl_shared_memory + eids.sstate_offset;
	__evl_current = eids.fundle;

	return efd;
}
