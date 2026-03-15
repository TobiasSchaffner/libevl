/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_INTRINSINCS_SEM_H
#define _EVL_INTRINSINCS_SEM_H

#include <evl/intrinsics/monitor.h>

#ifdef __cplusplus
extern "C" {
#endif

void evli_init_sem(struct evli_monitor *sem,
		__u32 sstate_offset);

int evli_tryget_sem(struct evli_monitor *sem);

int evli_tryput_sem(struct evli_monitor *sem, __s32 *sigval);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_INTRINSINCS_SEM_H */
