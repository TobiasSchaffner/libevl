/*
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <time.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <evl/compiler.h>
#include <evl/thread.h>
#include <evl/event.h>
#include <evl/mutex.h>
#include <evl/clock.h>
#include "helpers.h"

static struct evl_mutex lock;

static struct evl_event event;

static bool done;

#define HINT	"HINT: time to update your kernel?\n"	\
		"      make sure you have the following kernel patch in:\n" \
		"  ->  evl/monitor: fix missed wakeup on implicit gate release\n"

static void *wait_loop(int limit)
{
	struct timespec timeout;
	int ret;

	for (;;) {
		__Tcall_assert(ret, evl_lock_mutex(&lock));
		if (done)
			break;
		__Tcall_assert(ret, evl_signal_event(&event));
		/*
		 * Broken kernels would block the caller indefinitely
		 * on evl_timedwait_event() upon entering a race. Time
		 * out after one sec to detect those.
		 */
		__Tcall_assert(ret, evl_read_clock(EVL_CLOCK_MONOTONIC, &timeout));
		timeout.tv_sec += 1;
		if (!__Tcall(ret, evl_timedwait_event(&event, &lock, &timeout))) {
			evl_eprintf("%s", HINT);
			__Texpr_assert(0);
		}
		if (limit && --limit == 0) {
			__Tcall_assert(ret, evl_signal_event(&event));
			done = true;
			break;
		}
		__Tcall_assert(ret, evl_unlock_mutex(&lock));
		__Tcall_assert(ret, evl_usleep(1000));
	}

	__Tcall_assert(ret, evl_unlock_mutex(&lock));

	return NULL;
}

static void *event_waiter(void *arg)
{
	int tfd;

	__Tcall_assert(tfd, evl_attach_self("monitor-event-waiter:%d", getpid()));

	return wait_loop(0);
}

int main(int argc, char *argv[])
{
	struct sched_param param;
	int tfd, evfd, mfd, ret;
	void *status = NULL;
	pthread_t waiter;
	char *name;

	param.sched_priority = 1;
	__Texpr_assert(pthread_setschedparam(pthread_self(),
				SCHED_FIFO, &param) == 0);

	/* EVL inherits the inband scheduling params upon attachment. */
	__Tcall_assert(tfd, evl_attach_self("monitor-event-sigrel:%d", getpid()));

	name = get_unique_name(EVL_MONITOR_DEV, 0);
	__Tcall_assert(evfd, evl_new_event(&event, "%s", name));

	name = get_unique_name(EVL_MONITOR_DEV, 1);
	__Tcall_assert(mfd, evl_new_mutex(&lock, "%s", name));

	new_thread(&waiter, SCHED_FIFO, 1, event_waiter, NULL);

	__Texpr_assert(wait_loop(1200) == NULL);
	__Texpr_assert(pthread_join(waiter, &status) == 0);
	__Texpr_assert(status == NULL);

	__Tcall_assert(ret, evl_close_event(&event));
	__Tcall_assert(ret, evl_close_mutex(&lock));
}
