/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_INTRINSINCS_GATE_H
#define _EVL_INTRINSINCS_GATE_H

#include <evl/intrinsics/monitor.h>

#ifdef __cplusplus
extern "C" {
#endif

void evli_init_gate(struct evli_monitor *gate,
		__u32 sstate_offset);

int evli_trylock_gate(struct evli_monitor *gate);

int evli_tryunlock_gate(struct evli_monitor *gate);

int evli_set_gate_ceiling(struct evli_monitor *gate,
			unsigned int ceiling);

int evli_get_gate_ceiling(struct evli_monitor *gate);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_INTRINSINCS_GATE_H */
