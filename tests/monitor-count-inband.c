/*
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <time.h>
#include <poll.h>
#include <semaphore.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <evl/evl.h>
#include <evl/flags.h>
#include "helpers.h"

#define ROUNDS     1000

#define LOW_PRIO   1

static int i_ffd, o_ffd;

static struct evl_sem i_sem, o_sem;

static sem_t i_start, o_start;

static void *inband_receiver(void *arg)
{
	int n = 0, ret;
	__s32 count;

	__Tcall_assert(ret, sem_post(&i_start));

	do {
		__Tcall_assert(ret, read(i_ffd, &count, sizeof(count)));
		__Texpr_assert(count == 1);
	} while (++n < ROUNDS);

	return NULL;
}

static void *oob_receiver(void *arg)
{
	int n = 0, tfd, ret;
	__s32 count;

	__Tcall_assert(tfd, evl_attach_self("monitor-count-oob-receiver:%d",
					    getpid()));
	__Tcall_assert(ret, sem_post(&o_start));

	do {
		/*
		 * Exercise both oob interfaces: the evl_*_sem()
		 * routines on odd rounds, the oob I/O syscalls on
		 * even ones. The result should be the same: unicast
		 * signal.
		 */
		if (n % 1) {
			__Tcall_assert(ret, evl_get_sem(&i_sem));
			__Tcall_assert(ret, evl_put_sem(&o_sem));
		} else {
			__Tcall_assert(ret, oob_read(i_ffd, &count, sizeof(count)));
			__Texpr_assert(count == 1);
			__Tcall_assert(ret, oob_write(o_ffd, &count, sizeof(count)));
		}
	} while (++n < ROUNDS);

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t i_receiver, o_receiver;
	void *status;
	char *name;
	int n, ret;

	sem_init(&i_start, 0, 0);

	__Tcall_assert(ret, evl_init());
	name = get_unique_name(EVL_MONITOR_DEV, 0);
	__Tcall_assert(i_ffd, evl_new_sem(&i_sem, "%s", name));
	name = get_unique_name(EVL_MONITOR_DEV, 1);
	__Tcall_assert(o_ffd, evl_new_sem(&o_sem, "%s", name));

	new_thread(&i_receiver, SCHED_OTHER, 0, inband_receiver, NULL);
	__Tcall_assert(ret, sem_wait(&i_start));

	/* Try in-band -> in-band */

	for (n = 0; n < ROUNDS; n++) {
		__s32 count = 1;
		__Tcall_assert(ret, write(i_ffd, &count, sizeof(count)));
	}

	__Texpr_assert(pthread_join(i_receiver, &status) == 0);
	__Texpr_assert(status == NULL);

	new_thread(&o_receiver, SCHED_FIFO, 1, oob_receiver, NULL);
	__Tcall_assert(ret, sem_wait(&o_start));

	/* Try in-band -> oob and back. */

	for (n = 0; n < ROUNDS; n++) {
		__s32 count = 1;
		/* Post to oob. */
		__Tcall_assert(ret, write(i_ffd, &count, sizeof(count)));
		/* Wait for in-band echo. */
		__Tcall_assert(ret, read(o_ffd, &count, sizeof(count)));
		__Texpr_assert(count == 1);
	}

	__Texpr_assert(pthread_join(o_receiver, &status) == 0);
	__Texpr_assert(status == NULL);

	__Tcall_assert(ret, close(i_ffd));
	__Tcall_assert(ret, close(o_ffd));

	return 0;
}
