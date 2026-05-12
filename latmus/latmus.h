/*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_LATMUS_LATMUS_H
#define _EVL_LATMUS_LATMUS_H

#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>

struct in_addr;

#define ISOLATED_CPU_LIST "/sys/devices/system/cpu/isolated"
#define OOB_CPU_LIST	  "/sys/devices/virtual/evl/control/cpus"

#define ONE_BILLION	1000000000
#define TEN_MILLIONS	10000000

/* in-band vs oob modes for gpio and net tests (must be non-zero) */
#define	INBAND_MODE	1
#define OOB_MODE	2

#define EVL_LAT_OOB_GPIO      (EVL_LAT_LAST + 1)
#define EVL_LAT_INBAND_GPIO   (EVL_LAT_OOB_GPIO + 1)
#define EVL_LAT_NET           (EVL_LAT_INBAND_GPIO + 1)

void notify_start(int delay);

void parse_host_spec(const char *host,
		struct in_addr *in_addr);

int find_host_ip(const char *host,
		struct in_addr *addr);

int find_netif_ip(const char *netif,
		struct in_addr *addr);

const char *get_refclock_name(void);

void create_responder(pthread_t *tid,
		int priority, void *(*responder)(void *));

void create_logger(pthread_t *tid,
		void *(*logger)(void *), void *arg);

extern int test_irqlat, test_klat,
	test_ulat, test_sirqlat,
	test_gpiolat, test_netlat;

extern cpu_set_t isolated_cpus;

extern sigset_t sigmask;

extern int verbosity,
	abort_threshold,
	no_check,
	data_lines;

extern size_t packet_size;

extern time_t timeout;

extern bool abort_on_switch, c_state_restricted;

extern unsigned int spurious_inband_switches;

extern size_t histogram_cells;

extern time_t start_time;

extern FILE *plot_fp;

extern int context_type;

extern const char *context_labels[];

extern int responder_priority,
	responder_cpu,
	responder_cpu_state;

extern unsigned int period_usecs;

extern const char *peer_host;

extern const char *local_netif;

extern clockid_t reference_clock;

extern int latmus_fd;

#endif /* !_EVL_LATMUS_LATMUS_H */
