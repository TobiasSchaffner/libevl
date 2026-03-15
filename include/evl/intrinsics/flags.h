/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_INTRINSINCS_FLAGS_H
#define _EVL_INTRINSINCS_FLAGS_H

#include <evl/intrinsics/monitor.h>

#ifdef __cplusplus
extern "C" {
#endif

void evli_init_flags(struct evli_monitor *fgroup,
		__u32 sstate_offset);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_INTRINSINCS_FLAGS_H */
