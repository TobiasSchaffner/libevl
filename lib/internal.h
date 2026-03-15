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

#define __evl_conforming_io(__efd, __call, __args...)		\
	({							\
		int __ret;					\
		if (evli_is_inband())				\
			__ret = __call(__efd, ##__args);	\
		else						\
			__ret = oob_##__call(__efd, ##__args);	\
		__ret ? -errno : 0;				\
	})

int __evl_arch_init(void);

int __evl_attach_clocks(void);

void __evl_setup_proxies(void);

extern int (*__evl_clock_gettime)(clockid_t clk_id,
				struct timespec *tp);

extern void *__evl_shared_memory;

extern int __evl_ctlfd;

extern int __evl_mono_clockfd;

extern int __evl_real_clockfd;

#endif /* _LIB_EVL_INTERNAL_H */
