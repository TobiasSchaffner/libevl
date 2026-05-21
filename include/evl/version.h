/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_VERSION_H
#define _EVL_VERSION_H

#include <evl/control-abi.h>

/* The earliest ABI revision we can work with. */
#define EVL_ABI_PREREQ  44

#if EVL_ABI_LEVEL < EVL_ABI_PREREQ
#error EVL kernel uapi is too old
#endif

#define __EVL__  35	/* API version */

struct evl_version {
	int api_level;	/* libevl.so: __EVL__ */
	int abi_level;	/* EVL_ABI_PREREQ */
	const char *version_string;
};

#ifdef __cplusplus
extern "C" {
#endif

struct evl_version evl_get_version(void);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_VERSION_H */
