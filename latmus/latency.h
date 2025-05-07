#ifndef _EVL_LATMUS_LATENCY_H
#define _EVL_LATMUS_LATENCY_H

#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <signal.h>
#include <evl/devices/latmus.h>

struct in_addr;

#define ISOLATED_CPU_LIST "/sys/devices/system/cpu/isolated"
#define OOB_CPU_LIST	  "/sys/devices/virtual/evl/control/cpus"

#define ONE_BILLION	1000000000
#define TEN_MILLIONS	10000000

#define EVL_LAT_OOB_GPIO      (EVL_LAT_LAST + 1)
#define EVL_LAT_INBAND_GPIO   (EVL_LAT_OOB_GPIO + 1)

struct latmus_measurement;

void create_responder(pthread_t *tid,
		int priority, void *(*responder)(void *));

void create_logger(pthread_t *tid,
		void *(*logger)(void *), void *arg);

void __log_results(struct latmus_measurement *meas);

void log_results(struct latmus_measurement *meas,
		unsigned int round);

void notify_start(void);

void parse_host_spec(const char *host,
		struct in_addr *in_addr);

void do_measurement(int type, bool threaded, bool oob_mode);

extern cpu_set_t isolated_cpus;

extern sigset_t sigmask;

extern int verbosity,
	abort_threshold,
	data_lines;

extern time_t timeout;

extern bool abort_on_switch, c_state_restricted;

extern pthread_t responder, logger;

extern unsigned int all_overruns, spurious_inband_switches;

extern int32_t all_minlat, all_maxlat, *histogram;

extern int64_t all_sum, all_samples;

extern size_t histogram_cells;

extern time_t start_time, peak_time;

extern FILE *plot_fp;

extern int context_type;

extern const char *context_labels[];

extern int responder_priority,
	responder_cpu,
	responder_cpu_state;

extern unsigned int period_usecs;

extern sem_t logger_done;

extern int latmus_fd;

#endif /* !_EVL_LATMUS_LATENCY_H */
