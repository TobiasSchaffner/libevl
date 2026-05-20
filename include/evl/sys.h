/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_SYS_H
#define _EVL_SYS_H

#include <stdarg.h>
#include <signal.h>

struct evl_element_ids;

#ifdef __cplusplus
extern "C" {
#endif

int evl_init(void);

int evl_create_element(const char *type,
		       const char *name,
		       void *attrs,
		       int clone_flags,
		       struct evl_element_ids *eids);

int evl_open_element_vargs(const char *type,
			const char *fmt, va_list ap);

int evl_open_element(const char *type,
		     const char *path, ...);

int evl_open_raw(const char *type);

unsigned int evl_detect_fpu(void);

void evl_sigdebug_handler(int sig, siginfo_t *si, void *ctxt);

struct evl_version evl_get_version(void);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_SYS_H */
