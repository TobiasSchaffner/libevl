/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018-2021 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _LIB_EVL_ARM_SYSCALL_H
#define _LIB_EVL_ARM_SYSCALL_H

#include <asm-generic/dovetail.h>
#include <sys/syscall.h>

#define evl_syscall3(__nr, __a0, __a1, __a2)				\
({									\
		register unsigned long __sc __asm__("r7") = __NR_prctl;	\
		register unsigned long __res __asm__("r0");		\
		register unsigned long __r0  __asm__("r0");		\
		register unsigned long __r1  __asm__("r1");		\
		register unsigned long __r2  __asm__("r2");		\
		register unsigned long __r3  __asm__("r3");		\
		__r0 = (unsigned long)(__nr)|__OOB_SYSCALL_BIT;		\
		__r1 = (unsigned long)(__a0);				\
		__r2 = (unsigned long)(__a1);				\
		__r3 = (unsigned long)(__a2);				\
		__asm__ __volatile__ (					\
			"swi #0;\n\t"					\
			: "=r" (__res)					\
			: "r" (__sc),					\
			  "r" (__r0), "r" (__r1),			\
			  "r" (__r2), "r" (__r3)			\
			: "cc", "memory");				\
		__res;							\
	})

#endif /* !_LIB_EVL_ARM_SYSCALL_H */
