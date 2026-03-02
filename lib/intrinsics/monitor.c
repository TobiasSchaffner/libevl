/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018, 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <evl/intrinsics/monitor.h>
#include <evl/atomic.h>
#include "internal.h"

fundle_t evli_monitor_fundle(struct evli_monitor *mon)
{
	struct __evl_monitor_sstate *sstate;

	sstate = __evl_shared_memory + mon->sstate_offset;

	return sstate->shdr.fundle;
}


__u32 evli_monitor_value(struct evli_monitor *mon)
{
	struct __evl_monitor_sstate *sstate;

	sstate = __evl_shared_memory + mon->sstate_offset;

	return atomic_load(&sstate->u.event.value);
}
