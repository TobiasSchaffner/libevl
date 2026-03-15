/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018, 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <sched.h>
#include <evl/compiler.h>
#include <evl/atomic.h>
#include <evl/intrinsics/flags.h>
#include "internal.h"

void evli_init_flags(struct evli_monitor *fgroup, __u32 sstate_offset)
{
	struct __evl_monitor_sstate *sstate;

	fgroup->sstate_offset = sstate_offset;
	sstate = __evl_shared_memory + sstate_offset;
	/* Force commit the PTEs. */
	__force_pte_fixup(sstate->u.event.value);
	__force_pte_fixup(sstate->u.event.pollrefs);
}
