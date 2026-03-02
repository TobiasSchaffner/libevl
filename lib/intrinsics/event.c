/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018, 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <sched.h>
#include <evl/compiler.h>
#include <evl/atomic.h>
#include <evl/intrinsics/event.h>
#include "internal.h"

void evli_init_event(struct evli_monitor *event, __u32 sstate_offset)
{
	struct __evl_monitor_sstate *sstate;

	event->sstate_offset = sstate_offset;
	sstate = __evl_shared_memory + sstate_offset;
	/* Force commit the PTEs. */
	__force_pte_fixup(sstate->flags);
	__force_pte_fixup(sstate->u.event.gate_offset);
}

static struct __evl_monitor_sstate *
get_lock_state(struct evli_monitor *event)
{
	struct __evl_monitor_sstate *est = __evl_shared_memory + event->sstate_offset;

	if (est->u.event.gate_offset == EVL_MONITOR_NOGATE)
		return NULL;	/* Nobody waits on this event */

	return __evl_shared_memory + est->u.event.gate_offset;
}

void evli_signal_event(struct evli_monitor *event)
{
	struct __evl_monitor_sstate *est, *gst;

	gst = get_lock_state(event);
	if (gst) {
		gst->flags.signaled = true;
		est = __evl_shared_memory + event->sstate_offset;
		est->flags.signaled = true;
	}
}

int evli_signal_targeted(struct evli_monitor *event)
{
	struct __evl_monitor_sstate *gst;

	gst = get_lock_state(event);
	if (gst) {
		gst->flags.signaled = true;
		return -ENODATA;
	}

	/* No thread waits on this event. */

	return 0;
}

void evli_broadcast_event(struct evli_monitor *event)
{
	struct __evl_monitor_sstate *est, *gst;

	gst = get_lock_state(event);
	if (gst) {
		gst->flags.signaled = true;
		est = __evl_shared_memory + event->sstate_offset;
		est->flags.signaled = true;
		est->flags.broadcast = true;
	}
}
