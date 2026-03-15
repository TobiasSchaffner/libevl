/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018, 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <sched.h>
#include <evl/compiler.h>
#include <evl/atomic.h>
#include <evl/intrinsics/sem.h>
#include "internal.h"

void evli_init_sem(struct evli_monitor *sem, __u32 sstate_offset)
{
	struct __evl_monitor_sstate *sstate;

	sem->sstate_offset = sstate_offset;
	sstate = __evl_shared_memory + sstate_offset;
	/* Force commit the PTEs. */
	__force_pte_fixup(sstate->u.event.value);
	__force_pte_fixup(sstate->u.event.pollrefs);
}

int evli_tryget_sem(struct evli_monitor *sem)
{
	struct __evl_monitor_sstate *sst;
	__s32 val;

	sst = __evl_shared_memory + sem->sstate_offset;

	val = atomic_load_explicit(&sst->u.event.value, __ATOMIC_ACQUIRE);
	do {
		if (val <= 0)
			return -EAGAIN;
	} while (!atomic_compare_exchange_weak_explicit(
			&sst->u.event.value, &val, val - 1,
			__ATOMIC_RELEASE, __ATOMIC_ACQUIRE));

	return 0;
}

static inline bool is_polled(struct __evl_monitor_sstate *sst)
{
	return !!atomic_load(&sst->u.event.pollrefs);
}

int evli_tryput_sem(struct evli_monitor *sem, __s32 *sigval)
{
	struct __evl_monitor_sstate *sst;
	__s32 val;

	sst = __evl_shared_memory + sem->sstate_offset;

	*sigval = 1;

	val = atomic_load_explicit(&sst->u.event.value, __ATOMIC_ACQUIRE);
	if (val < 0 || is_polled(sst))
		return -ENODATA;

	while (!atomic_compare_exchange_weak_explicit(
			&sst->u.event.value, &val, val + 1,
			__ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
		if (val < 0)
			return -ENODATA;
	}

	if (is_polled(sst)) {
		*sigval = 0;
		return -ENODATA;
	}

	return 0;
}
