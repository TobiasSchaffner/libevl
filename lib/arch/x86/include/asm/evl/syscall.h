/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2019-2021 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _LIB_EVL_X86_SYSCALL_H
#define _LIB_EVL_X86_SYSCALL_H

#include <asm-generic/dovetail.h>
#include <sys/syscall.h>

#define evl_syscall3(__nr, __a0, __a1, __a2)				\
	({								\
		register long int _nr asm ("rdi") = (__nr) | __OOB_SYSCALL_BIT; \
		register long int _a0 asm ("rsi") = (long)(__a0);	\
		register long int _a1 asm ("rdx") = (long)(__a1);	\
		register long int _a2 asm ("r10") = (long)(__a2);	\
		register long int _a3 asm ("r8") = 0L;			\
		long int __res;						\
		__asm__ __volatile__ (					\
			"syscall;\n\t"					\
			: "=a" (__res)					\
			: "0" (__NR_prctl),				\
			  "r" (_nr),					\
			  "r" (_a0), "r" (_a1), "r" (_a2), "r" (_a3)	\
			: "cc", "memory", "r11", "cx");			\
		(int)__res;						\
	})

#endif /* !_LIB_EVL_X86_SYSCALL_H */
