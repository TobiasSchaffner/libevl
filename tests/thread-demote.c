/*
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>
#include <evl/thread.h>
#include <evl/sem.h>
#include <evl/clock.h>
#include "helpers.h"

#define LOW_PRIO   1
#define HIGH_PRIO  2

static struct evl_sem start;

static void *runaway_thread(void *arg)
{
	int *ptfd = (int *)arg, ret;

	__Tcall_assert(*ptfd, evl_attach_self("runaway:%d", getpid()));

	__Tcall_assert(ret, evl_put_sem(&start));
	__Tcall_assert(ret, evl_get_sem(&start));

	while (!evl_is_inband())
		;

	return NULL;
}

static void spawn_runaway_thread(void)
{
	pthread_t tid;
	int tfd, ret;

	new_thread(&tid, SCHED_FIFO, LOW_PRIO, runaway_thread, &tfd);

	__Tcall_assert(ret, evl_get_sem(&start));
	__Tcall_assert(ret, evl_put_sem(&start));
	__Tcall_assert(ret, evl_usleep(50000));
	__Tcall_assert(ret, evl_demote_thread(tfd));
	pthread_cancel(tid);
	pthread_join(tid, NULL);
	/*
	 * Closing this file descriptor eventually releases the
	 * remaining thread resources in the core, allowing for the
	 * final disposal process to take place.
	 */
	close(tfd);
}

int main(int argc, char *argv[])
{
	struct sched_param param;
	int ret, tfd, sfd, n;
	cpu_set_t affinity;
	char *name;

	CPU_ZERO(&affinity);
	CPU_SET(0, &affinity);
	__Tcall_assert(ret, sched_setaffinity(0, sizeof(affinity), &affinity));

	param.sched_priority = HIGH_PRIO;
	__Texpr_assert(pthread_setschedparam(pthread_self(),
				SCHED_FIFO, &param) == 0);
	__Tcall_assert(tfd, evl_attach_self("thread-demote:%d", getpid()));

	name = get_unique_name(EVL_MONITOR_DEV, 0);
	__Tcall_assert(sfd, evl_new_sem(&start, "%s", name));

	/*
	 * Check that we can respawn a thread multiple times within a
	 * short time window without any issue, typically no EEXIST
	 * error due to the asynchronous nature of (thread) element
	 * disposal.
	 */
	for (n = 0; n < 20; n++)
		spawn_runaway_thread();

	return 0;
}
