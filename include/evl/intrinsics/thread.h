/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_INTRINSINCS_THREAD_H
#define _EVL_INTRINSINCS_THREAD_H

#include <stdarg.h>
#include <stdbool.h>
#include <evl/thread-abi.h>

struct evli_thread {
	__u32 sstate_offset;
};

/* Enable dlopen() on libevl.so. */
#define EVL_TLS_MODEL	"global-dynamic"

extern __thread __attribute__ ((tls_model (EVL_TLS_MODEL)))
fundle_t __evl_current;

extern __thread __attribute__ ((tls_model (EVL_TLS_MODEL)))
struct __evl_thread_sstate *__evl_current_sstate;

#ifdef __cplusplus
extern "C" {
#endif

static inline int evli_current_mode(void)
{
	return __evl_current_sstate ?
		__evl_current_sstate->state : EVL_T_INBAND;
}

static inline fundle_t evli_current(void)
{
	return __evl_current;
}

static inline struct __evl_thread_sstate *
evli_current_sstate(void)
{
	return __evl_current ? __evl_current_sstate : NULL;
}

static inline bool evli_is_inband(void)
{
	return !!(evli_current_mode() & EVL_T_INBAND);
}

int evli_attach_thread(struct evli_thread *thread,
		int flags, const char *fmt, va_list ap);

void evli_clear_tls(void);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_INTRINSINCS_THREAD_H */
