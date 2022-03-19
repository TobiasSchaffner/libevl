/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_ATOMIC_H
#define _EVL_ATOMIC_H

#include <linux/types.h>

#ifndef __cplusplus

#include <stdatomic.h>

/*
 * We use C11 atomic functions solely with the 32bit word type defined
 * by the kernel ABI.
 */
typedef _Atomic(__s32) atomic_t;

/*
 * A couple of wrappers matching the kernel atomic API used in the
 * UAPI bits to the C11 interface. We assume read-cmpxchg are paired.
 */
#define atomic_read(__ptr)	atomic_load_explicit(__ptr, __ATOMIC_ACQUIRE)

#define atomic_cmpxchg(__ptr, __oldval, __newval)			\
	({								\
		typeof(__oldval) __exp = (__oldval);			\
		typeof(__newval) __des = (__newval);			\
		atomic_compare_exchange_strong_explicit(		\
			__ptr, &__exp, __des,				\
			__ATOMIC_RELEASE, __ATOMIC_ACQUIRE);		\
		__exp;							\
	})

#else /* __cplusplus */

/*
 * C11 and C++ atomic APIs do not mix well at the moment. We define a
 * fake atomic type of the right size only to allow C++ code to build
 * properly when including this file, although we do not expect any
 * actual atomic operation to be performed. Data definitions including
 * atomic types are still done right though.
 */
typedef __s32 atomic_t;

#define atomic_read(__ptr)				__c11_vs_cplusplus_atomic_mismatch()
#define atomic_cmpxchg(__ptr, __oldval, __newval)	__c11_vs_cplusplus_atomic_mismatch()

#endif	/* __cplusplus */

#endif /* _EVL_ATOMIC_H */
