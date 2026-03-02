/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_INTRINSINCS_EVENT_H
#define _EVL_INTRINSINCS_EVENT_H

#include <evl/intrinsics/monitor.h>

#ifdef __cplusplus
extern "C" {
#endif

void evli_init_event(struct evli_monitor *event,
		__u32 sstate_offset);

void evli_signal_event(struct evli_monitor *event);

int evli_signal_targeted(struct evli_monitor *event);

void evli_broadcast_event(struct evli_monitor *event);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_INTRINSINCS_EVENT_H */
