/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2019 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_MUTEX_H
#define _EVL_MUTEX_H

#include <time.h>
#include <evl/compiler.h>
#include <evl/intrinsics/gate.h>

#define EVL_MUTEX_NORMAL     (0 << 0)
#define EVL_MUTEX_RECURSIVE  (1 << 0)

#define __MUTEX_UNINIT_MAGIC	0xfe11fe11
#define __MUTEX_ACTIVE_MAGIC	0xab12ab12

struct evl_mutex {
	unsigned int magic;
	struct evli_monitor gate;
	union {
		struct {
			int efd;
		} active;
		struct {
			const char *name;
			int clockfd;
			unsigned int ceiling;
			int flags;
		} uninit;
	} u;
};

#define EVL_MUTEX_INITIALIZER(__name, __clockfd, __ceiling, __flags)	\
	(struct evl_mutex) {						\
		.magic = __MUTEX_UNINIT_MAGIC,				\
		.u = {							\
			.uninit = {					\
				.name = (__name),			\
				.clockfd = (__clockfd),			\
				.ceiling = (__ceiling),			\
				.flags = (__flags),			\
			}						\
		}							\
	}

#define DEFINE_EVL_MUTEX(__name)					\
  	struct evl_mutex __name =					\
	  EVL_MUTEX_INITIALIZER(#__name, EVL_CLOCK_MONOTONIC,		\
				0, EVL_MUTEX_NORMAL|EVL_CLONE_PRIVATE)

#define evl_new_mutex(__mutex, __fmt, __args...)		\
	evl_create_mutex(__mutex, EVL_CLOCK_MONOTONIC,		\
			0, EVL_MUTEX_NORMAL|EVL_CLONE_PRIVATE,	\
			__fmt, ##__args)

#ifdef __cplusplus
extern "C" {
#endif

int evl_create_mutex(struct evl_mutex *mutex,
		int clockfd, unsigned int ceiling, int flags,
		const char *fmt, ...) __check_printf(5, 6);

int evl_open_mutex(struct evl_mutex *mutex,
		const char *fmt, ...) __check_printf(2, 3);

int evl_lock_mutex(struct evl_mutex *mutex);

int evl_timedlock_mutex(struct evl_mutex *mutex,
			const struct timespec *timeout);

int evl_trylock_mutex(struct evl_mutex *mutex);

int evl_unlock_mutex(struct evl_mutex *mutex);

int evl_set_mutex_ceiling(struct evl_mutex *mutex,
			unsigned int ceiling);

int evl_get_mutex_ceiling(struct evl_mutex *mutex);

int evl_close_mutex(struct evl_mutex *mutex);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_MUTEX_H */
