/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _LIB_EVL_INTERNAL_H
#define _LIB_EVL_INTERNAL_H

#include <time.h>
#include <stdint.h>
#include <evl/intrinsics/thread.h>

#define __evl_ptr64(__ptr)		((__u64)(uintptr_t)(__ptr))
#define __evl_ktimespec_ptr64(__ts)	__evl_ptr64(__ts)
#define __evl_kitimerspec_ptr64(__its)	__evl_ptr64(__its)

extern __thread __attribute__ ((tls_model (EVL_TLS_MODEL)))
int __evl_current_efd;

/*
 * A transparent EVL call does not cause any stage switch. Works only
 * if the syscall is implemented identically for the in-band and
 * out-of-band stages.
 */
#define __evl_transparent_call(__efd, __call, __args...)	\
	({							\
		int __ret;					\
		if (evli_is_inband())				\
			__ret = __call(__efd, ##__args);	\
		else						\
			__ret = oob_##__call(__efd, ##__args);	\
		__ret ? -errno : 0;				\
	})

/*
 * A conforming EVL call uses the most appropriate I/O service for the
 * caller, based on its scheduling information. As a result, it may
 * cause a switch to the out-of-band stage. Works only if the syscall
 * is implemented identically for the in-band and out-of-band stages.
 */
#define __evl_conforming_call(__efd, __call, __args...)			\
	({								\
		int __ret, __mode = evli_current_mode();		\
		fundle_t __current = evli_current();			\
		if (!__current || (__mode & (EVL_T_WEAK|EVL_T_INBAND))	\
			== (EVL_T_WEAK|EVL_T_INBAND))			\
			__ret = __call(__efd, ##__args);		\
		else							\
			__ret = oob_##__call(__efd, ##__args);		\
		__ret ? -errno : 0;					\
	})

int __evl_arch_init(void);

int __evl_attach_clocks(void);

void __evl_setup_proxies(void);

extern int (*__evl_clock_gettime)(clockid_t clk_id,
				struct timespec *tp);

extern void *__evl_shared_memory;

extern int __evl_ctlfd;

extern int __evl_mono_clockfd;

extern int __evl_mono_raw_clockfd;

extern int __evl_real_clockfd;

#endif /* _LIB_EVL_INTERNAL_H */
