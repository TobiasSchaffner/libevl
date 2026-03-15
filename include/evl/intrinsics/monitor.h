/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_INTRINSINCS_MONITOR_H
#define _EVL_INTRINSINCS_MONITOR_H

#include <evl/monitor-abi.h>

struct evli_monitor {
	__u32 sstate_offset;
};

#ifdef __cplusplus
extern "C" {
#endif

fundle_t evli_monitor_fundle(struct evli_monitor *mon);

__u32 evli_monitor_value(struct evli_monitor *mon);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_INTRINSINCS_MONITOR_H */
