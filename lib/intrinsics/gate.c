/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018, 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <sched.h>
#include <evl/compiler.h>
#include <evl/atomic.h>
#include <evl/intrinsics/gate.h>
#include "internal.h"

static __always_inline atomic_t *__ATOMIC32(__u32 *ptr)
{
	return (atomic_t *)ptr;
}

static inline
int atomic_trylock_gate(__u32 *lock, fundle_t new)
{
	fundle_t old;

	old = atomic_cmpxchg(__ATOMIC32(lock), EVL_NO_HANDLE, new);
	if (old != EVL_NO_HANDLE) {
		if (__evl_fundle_key(old) == new)
			return -EBUSY;

		return -EAGAIN;
	}

	return 0;
}

static inline
bool atomic_tryunlock_gate(__u32 *lock, fundle_t old)
{
	return atomic_cmpxchg(__ATOMIC32(lock), old, EVL_NO_HANDLE) == old;
}

static inline bool is_gate_owner(__u32 *lock, fundle_t me)
{
	return __evl_fundle_key(atomic_read(__ATOMIC32(lock))) == me;
}

void evli_init_gate(struct evli_monitor *gate, __u32 sstate_offset)
{
	struct __evl_monitor_sstate *sstate;

	gate->sstate_offset = sstate_offset;
	sstate = __evl_shared_memory + sstate_offset;
	/* Force commit PTEs. */
	__force_pte_fixup(sstate->flags);
	__force_pte_fixup(sstate->u.gate.owner);
}

int evli_trylock_gate(struct evli_monitor *gate)
{
	struct __evl_thread_sstate *sstate;
	struct __evl_monitor_sstate *gst;
	bool protect = false;
	fundle_t current;
	int mode, ret;

	current = __evl_get_current();
	if (current == EVL_NO_HANDLE)
		return -EPERM;

	gst = __evl_shared_memory + gate->sstate_offset;

	/*
	 * Threads running in-band and/or enabling WOLI debug must go
	 * through the slow syscall path.
	 */
	mode = __evl_get_current_mode();
	if (!(mode & (EVL_T_INBAND|EVL_T_WEAK|EVL_T_WOLI))) {
		if (gst->protocol == EVL_GATE_PP) {
			sstate = __evl_get_current_sstate();
			/*
			 * Can't nest lazy ceiling requests, have to
			 * take the slow path when this happens.
			 */
			if (sstate->pp_pending != EVL_NO_HANDLE)
				goto slow_path;
			sstate->pp_pending = gst->shdr.fundle;
			protect = true;
		}
		ret = atomic_trylock_gate(&gst->u.gate.owner, current);
		if (ret == 0) {
			gst->u.gate.nesting = 1;
			gst->flags.signaled = false;
			return 0;
		}
	} else {
	slow_path:
		ret = 0;
		if (is_gate_owner(&gst->u.gate.owner, current))
			ret = -EBUSY;
	}

	if (ret == -EBUSY) {
		if (protect)
			sstate->pp_pending = EVL_NO_HANDLE;

		if (gst->u.gate.recursive) {
			if (++gst->u.gate.nesting == 0) {
				gst->u.gate.nesting = ~0;
				return -EAGAIN;
			}
			return 0;
		}

		return -EDEADLK;
	}

	return -ENODATA;
}

/**
 * evli_tryunlock_gate - Try unlocking the gate atomically.
 *
 * Attempts a syscall-less, atomic unlock of the gate. This can be
 * achieved only if no thread is currently waiting for locking the
 * gate.
 *
 * Zero is returned on success. Otherwise,
 *
 * -ENODATA 	One or more threads are waiting for gate to be unlocked, or
 * 		the gate guards an event-type monitor some threads are
 * 		waiting on, or the current runtime mode requires the kernel
 *              to handle the release.  Either way, the caller should interpret
 *              this status as a requirement to take the slow/regular syscall
 *              path for unlocking the gate.
 *
 * -EPERM	The caller does not own the gate lock.
 */
int evli_tryunlock_gate(struct evli_monitor *gate)
{
	struct __evl_thread_sstate *sstate;
	struct __evl_monitor_sstate *gst;
	fundle_t current;
	int mode;

	gst = __evl_shared_memory + gate->sstate_offset;

	current = __evl_get_current();
	if (!is_gate_owner(&gst->u.gate.owner, current))
		return -EPERM;

	if (gst->u.gate.nesting > 1) {
		gst->u.gate.nesting--;
		return 0;
	}

	/* Do we have waiters on a signaled event we are gating? */
	if (gst->flags.signaled)
		return -ENODATA;

	mode = __evl_get_current_mode();
	if (mode & (EVL_T_WEAK|EVL_T_WOLI))
		return -ENODATA;

	if (atomic_tryunlock_gate(&gst->u.gate.owner, current)) {
		if (gst->protocol == EVL_GATE_PP) {
			sstate = __evl_get_current_sstate();
			sstate->pp_pending = EVL_NO_HANDLE;
		}
		return 0;
	}

	/*
	 * If the atomic unlock failed, somebody else must be waiting
	 * for entering the lock or PP was committed for the current
	 * thread. Need to ask the kernel for proper release.
	 */
	return -ENODATA;
}

int evli_set_gate_ceiling(struct evli_monitor *gate,
			unsigned int ceiling)
{
	struct __evl_monitor_sstate *gst;
	int ret;

	if (ceiling == 0)
		return -EINVAL;

	ret = sched_get_priority_max(SCHED_FIFO);
	if (ret < 0 || ceiling > (unsigned int)ret)
		return -EINVAL;

	gst = __evl_shared_memory + gate->sstate_offset;

	if (gst->protocol != EVL_GATE_PP)
		return -EINVAL;

	gst->u.gate.ceiling = ceiling;

	return 0;
}

int evli_get_gate_ceiling(struct evli_monitor *gate)
{
	struct __evl_monitor_sstate *gst;

	gst = __evl_shared_memory + gate->sstate_offset;

	if (gst->protocol != EVL_GATE_PP)
		return 0;

	return gst->u.gate.ceiling;
}
