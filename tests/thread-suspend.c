/*
 * SPDX-License-Identifier: MIT
 *
 * Test the behaviour of asynchronous thread suspension and
 * resumption.
 *
 * The test scenarios are strictly (thread) priority-dependent.  Each
 * suspend_*() thread puppeteers the associated *_{runner, waiter}()
 * threads by having them carrying out actions in
 * lock-step. Completing all of these actions with neither hang nor
 * crash proves the correctness of the core with respect to handling
 * asynchronous suspension/resumption requests.
 */

#include <sys/types.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <sched.h>
#include <fcntl.h>
#include <signal.h>
#include <evl/thread.h>
#include <evl/clock.h>
#include <evl/sem.h>
#include "helpers.h"

#define LOW_PRIO   1
#define HIGH_PRIO  2

static int sfd1, sfd2, sfd3;

static struct evl_sem sem1, sem2, sem3;

static sigset_t sigmask;

static void *oob_waiter(void *arg)
{
	int *ptfd = (int *)arg, ret;

	__Tcall_assert(*ptfd, evl_attach_self("oob-waiter:%d", getpid())); /* SCHED_FIFO */
	__Texpr_assert(!evl_is_inband());

	__Tcall_assert(ret, evl_put_sem(&sem1));
	__Tcall_assert(ret, evl_get_sem(&sem1));
	__Tcall_assert(ret, evl_put_sem(&sem2));

	return NULL;
}

static void *inband_waiter(void *arg)
{
	int *ptfd = (int *)arg, ret;
	__s32 count = 1;

	__Tcall_assert(*ptfd, evl_attach_self("inband-waiter:%d", getpid())); /* SCHED_WEAK */
	__Texpr_assert(evl_is_inband());

	__Tcall_assert(ret, write(sfd1, &count, sizeof count));
	__Tcall_assert(ret, read(sfd1, &count, sizeof count));
	__Texpr_assert(evl_is_inband()); /* Per SCHED_WEAK as we don't hold any mutex. */
	__Texpr_assert(count == 1);
	__Tcall_assert(ret, write(sfd2, &count, sizeof count));

	return NULL;
}

static void check_suspend_waiter(int *tfd)
{
	int ret;

	/* Wait for initial handshake. */
	__Tcall_assert(ret, evl_get_sem(&sem1));

	/* We should have preempted the waiter, suspend it. */
	__Tcall_assert(ret, evl_suspend_thread(*tfd));

	/*
	 * If actually suspended, the waiter can't deplete the
	 * semaphore until we resume it.
	 */
	__Tcall_assert(ret, evl_put_sem(&sem1));
	__Tcall_assert(ret, evl_tryget_sem(&sem1));

	/* Looks ok, check resumption now. */
	__Tcall_assert(ret, evl_resume_thread(*tfd));
	__Tcall_assert(ret, evl_put_sem(&sem1));

	/*
	 * The waiter should have resumed and unblocked from
	 * get(&sem1), wait for it to put(&sem2).
	 */
	__Tcall_assert(ret, evl_get_sem(&sem2));
}

static void test_suspend_oob_waiter(void)
{
	pthread_t tid;
	int tfd;

	new_thread(&tid, SCHED_FIFO, LOW_PRIO, oob_waiter, &tfd);
	check_suspend_waiter(&tfd);
	pthread_join(tid, NULL);
	close(tfd);
}

static void test_suspend_inband_waiter(void)
{
	pthread_t tid;
	int tfd;

	new_thread(&tid, SCHED_OTHER, 0, inband_waiter, &tfd);
	check_suspend_waiter(&tfd);
	pthread_join(tid, NULL);
	close(tfd);
}

static void sighup_handler(int sig, siginfo_t *si, void *context)
{
	int ret;

	__Tcall_assert(ret, evl_put_sem(&sem3));
}

static void *oob_runner(void *arg)
{
	int *ptfd = (int *)arg, ret;

	__Tcall_assert(*ptfd, evl_attach_self("oob-runner:%d", getpid())); /* SCHED_FIFO */
	__Texpr_assert(!evl_is_inband());

	/* We want to receive SIGHUP. */
	__Tcall_assert(ret, pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL));

	/*
	 * evl_put_sem() is a transparent call stage-wise, so we need
	 * to switch back out-of-band manually before unblocking the
	 * test driver for the next step.
	 */
	__Tcall_assert(ret, evl_switch_oob());
	__Tcall_assert(ret, evl_put_sem(&sem1));

	/*
	 * Start the spinning wait loop. Ends when the test driver
	 * signals sem1 unless we are suspended.
	 */
	for (;;) {
		ret = evl_tryget_sem(&sem1);
		if (ret != -EAGAIN) {
			__Texpr_assert(ret == 0);
			break;
		}
	}

	__Tcall_assert(ret, evl_put_sem(&sem2));

	__Tcall_assert(ret, pthread_sigmask(SIG_BLOCK, &sigmask, NULL));

	return NULL;
}

static void *inband_runner(void *arg)
{
	int *ptfd = (int *)arg, sfd, ret;
	__s32 count = 1;

	__Tcall_assert(*ptfd, evl_attach_self("inband-runner:%d", getpid())); /* SCHED_WEAK */
	__Texpr_assert(evl_is_inband());

	/* We want to receive SIGHUP. */
	__Tcall_assert(ret, pthread_sigmask(SIG_UNBLOCK, &sigmask, NULL));

	/*
	 * Prepare a non-blocking file descriptor for the spinning
	 * loop.
	 */
	sfd = dup(sfd1);
	__Tcall_assert(ret, fcntl(sfd, F_GETFL, 0));
	__Tcall_assert(ret, fcntl(sfd, F_SETFL, ret | O_NONBLOCK));

	/* Unblock the test driver for the next step. */
	__Tcall_assert(ret, write(sfd1, &count, sizeof count));

	/*
	 * Start the spinning wait loop. Ends when the test driver
	 * signals sem1 unless we are suspended.
	 */
	for (;;) {
		ret = read(sfd, &count, sizeof count);
		if (ret != -1) {
			__Texpr_assert(ret == sizeof count && count == 1);
			break;
		}
		__Texpr_assert(evl_is_inband()); /* Per SCHED_WEAK */
		__Texpr_assert(errno == EAGAIN);
	}
	__Texpr_assert(count == 1);

	__Tcall_assert(ret, write(sfd2, &count, sizeof count));
	close(sfd);

	__Tcall_assert(ret, pthread_sigmask(SIG_BLOCK, &sigmask, NULL));

	return NULL;
}

static void check_suspend_runner(int *tfd, pthread_t tid)
{
	int ret;

	/* Wait for initial handshake. */
	__Tcall_assert(ret, evl_get_sem(&sem1));

	/*
	 * Sleep a bit to let the runner kick in, then suspend it. If
	 * that works, the runner won't be able to deplete the
	 * semaphore until we resume it.
	 */
	__Tcall_assert(ret, evl_usleep(70000));
	__Tcall_assert(ret, evl_suspend_thread(*tfd));

	/*
	 * Make sure in-band signals play nicely with forcible suspend
	 * conditions on EVL threads (i.e. don't permanently override
	 * them when causing a stage demotion for handling).
	 */
	__Tcall_assert(ret, pthread_kill(tid, SIGHUP));

	/* Give some time for signal to be delivered. */
	__Tcall_assert(ret, evl_usleep(70000));
	__Tcall_assert(ret, evl_put_sem(&sem1));
	__Tcall_assert(ret, evl_usleep(70000));
	__Tcall_assert(ret, evl_tryget_sem(&sem1));

	/* Looks ok, check resumption now. */
	__Tcall_assert(ret, evl_resume_thread(*tfd));
	__Tcall_assert(ret, evl_put_sem(&sem1));

	/*
	 * The runner should have resumed and unblocked from
	 * [try]get(&sem1), wait for it to put(&sem2).
	 */
	__Tcall_assert(ret, evl_get_sem(&sem2));

	/* The signal should have been delivered. */
	__Tcall_assert(ret, evl_get_sem(&sem3));
}

static void test_suspend_oob_runner(void)
{
	pthread_t tid;
	int tfd;

	new_thread(&tid, SCHED_FIFO, LOW_PRIO, oob_runner, &tfd);
	check_suspend_runner(&tfd, tid);
	pthread_join(tid, NULL);
	close(tfd);
}

static void test_suspend_inband_runner(void)
{
	pthread_t tid;
	int tfd;

	new_thread(&tid, SCHED_OTHER, 0, inband_runner, &tfd);
	check_suspend_runner(&tfd, tid);
	pthread_join(tid, NULL);
	close(tfd);
}

int main(int argc, char *argv[])
{
	struct sched_param param;
	struct sigaction sa;
	cpu_set_t affinity;
	int ret, tfd;
	char *name;

	CPU_ZERO(&affinity);
	CPU_SET(0, &affinity);
	__Tcall_assert(ret, sched_setaffinity(0, sizeof(affinity), &affinity));

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGHUP);
	pthread_sigmask(SIG_BLOCK, &sigmask, NULL);

	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = sighup_handler;
	sa.sa_flags = 0;
	sigaction(SIGHUP, &sa, NULL);

	param.sched_priority = HIGH_PRIO;
	__Texpr_assert(pthread_setschedparam(pthread_self(),
				SCHED_FIFO, &param) == 0);
	__Tcall_assert(tfd, evl_attach_self("thread-suspend:%d", getpid()));

	name = get_unique_name(EVL_MONITOR_DEV, 0);
	__Tcall_assert(sfd1, evl_new_sem(&sem1, "%s", name));
	name = get_unique_name(EVL_MONITOR_DEV, 1);
	__Tcall_assert(sfd2, evl_new_sem(&sem2, "%s", name));
	name = get_unique_name(EVL_MONITOR_DEV, 2);
	__Tcall_assert(sfd3, evl_new_sem(&sem3, "%s", name));

	test_suspend_oob_waiter();
	test_suspend_inband_waiter();
	test_suspend_oob_runner();
	test_suspend_inband_runner();

	return 0;
}
