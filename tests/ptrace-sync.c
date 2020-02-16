/*
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <stdbool.h>
#include <regex.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <error.h>
#include <errno.h>
#include <evl/event.h>
#include <evl/sem.h>
#include <evl/sched.h>
#include <evl/thread.h>
#include <evl/atomic.h>
#include "helpers.h"

/*
 * This test checks that synchronized ptracing works, i.e.:
 *
 * 1. all threads belonging to the same process are immediately
 *    stopped from executing out-of-band whenever one of them hits a
 *    debugger breakpoint.
 *
 * 2. stopped threads resume when the stopped one resumes.
 *
 * 3. threads resume on the execution stage they were stopped on
 *    (oob vs in-band).
 *
 * 4. the out-of-band priority scheme is enforced among resuming
 *    threads (i.e. the high priority one resumes first and so on).
 */

#define CONCURRENCY  2

static struct evl_sem handshake;
static struct evl_event barrier;
static struct evl_mutex lock;
static bool started;
static uatomic_t who;

static void bp(void)
{
	__Texpr_assert(!evl_is_inband());
}

static void wait_release(void)
{
	int ret;

	__Tcall_assert(ret, evl_lock_mutex(&lock));
	for (;;) {
		if (started)
			break;
		__Tcall_assert(ret, evl_wait_event(&barrier, &lock));
	}
	__Tcall_assert(ret, evl_unlock_mutex(&lock));
}

static void *test_thread(void *arg)
{
	uatomic_t me = (uatomic_t)(long)(arg);
	int tfd, ret;

	__Tcall_assert(tfd, evl_attach_self("gdb-test:%d.%d", getpid(), me));
	__Tcall_assert(ret, evl_put_sem(&handshake)); /* Sync with main() */
	wait_release();
	atomic_store(&who, me);
	bp();
	__Texpr_assert(atomic_read(&who) == me);

	return NULL;
}

static int debuggee(void)
{
	struct evl_sched_attrs attrs;
	pthread_t tids[CONCURRENCY];
	cpu_set_t cpu_set;
	int tfd, ret, n;

	/* We must be attached to the core to lock the barrier. */
	__Tcall_assert(tfd, evl_attach_self("gdb-main:%d", getpid()));

	attrs.sched_policy = SCHED_WEAK;
	attrs.sched_priority = 0;
	__Tcall_assert(ret, evl_set_schedattr(tfd, &attrs));

	/* We need all test threads to inherit the same CPU affinity. */
	CPU_ZERO(&cpu_set);
	CPU_SET(0, &cpu_set);
	if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set)) {
		perror("sched_setaffinity");
		return 2;
	}

	__Tcall_assert(ret, evl_new_event(&barrier, "gdb-barrier:%d", getpid()));
	__Tcall_assert(ret, evl_new_mutex(&lock, "gdb-lock:%d", getpid()));
	__Tcall_assert(ret, evl_new_sem(&handshake, "handshake-sem:%d", getpid()));

	__Texpr_assert(evl_is_inband());
	__Tcall_assert(ret, evl_lock_mutex(&lock));
	__Tcall_assert(ret, evl_unlock_mutex(&lock));
	__Texpr_assert(evl_is_inband());

	for (n = 0; n < CONCURRENCY; n++) {
		new_thread(tids + n, SCHED_FIFO, n + 1, test_thread, (void *)(long)(n + 1));
		__Tcall_assert(ret, evl_get_sem(&handshake));
	}

	__Texpr_assert(evl_is_inband());

	/* Release all test threads. */
	__Tcall_assert(ret, evl_lock_mutex(&lock));
	started = true;
	__Tcall_assert(ret, evl_broadcast_event(&barrier));
	bp();
	__Tcall_assert(ret, evl_unlock_mutex(&lock));

	__Texpr_assert(evl_is_inband());

	for (n = 0; n < CONCURRENCY; n++)
		pthread_join(tids[n], NULL);

	return 0;
}

static struct dialog {
	const char *expect;
	const char *send;
} dialog[] = {
	{ "^Reading symbols from .*", "set prompt \\001(gdb)\\n\\002" },
	{ ".*(gdb).*", "set env __EVL_DEBUGGEE__=1" },
	{ "(gdb)", "b bp" },
	{ "^Breakpoint 1 at .*: bp\\.", "r" },
	{ "^Thread 1 .* hit Breakpoint 1.*, bp ().*", "c" },
	{ "^Thread 3 .* hit Breakpoint 1.*, bp ().*", "c" },
	{ "^Thread 2 .* hit Breakpoint 1.*, bp ().*", "c" },
	{ "^\\[Inferior .* exited normally\\]", "q" },
	{ NULL, NULL },
};

static int consume_and_match(FILE *fp, const char *expect, bool verbose)
{
	int ret = EXIT_FAILURE;
	char buf[BUFSIZ], *p;
	regex_t re;

	if (verbose)
		fprintf(stderr, "EXPECT {{ %s }}\n", expect);

	if (regcomp(&re, expect, REG_NEWLINE))
		return ret;

	for (;;) {
		alarm(10);	/* Bail out after 10s if unresponsive. */
		p = fgets(buf, sizeof(buf), fp);
		alarm(0);

		if (p) {
			if (verbose) {
				fprintf(stderr, "<- {{ \"%.*s\" }}\n", (int)(strlen(p) - 1), p);
				fflush(stderr);
			}
			if (!regexec(&re, p, 0, NULL, 0)) {
				if (verbose)
					fprintf(stderr, "MATCHED {{ %s }}\n", expect);
				fflush(stderr);
				ret = 0;
				break;	/* Yep, this is a match. */
			}
		}
	}

	regfree(&re);

	return ret;
}

static int send_next_command(FILE *fp, const char *send, bool verbose)
{
	int ret;

	ret = fputs(send, fp) == EOF;
	ret |= fputc('\n', fp) == EOF;
	fflush(fp);

	if (verbose) {
		fprintf(stderr, "-> {{ \"%s\" }}%s\n", send, ret ? " (FAILED)" : "");
		fflush(stderr);
	}

	return ret;
}

static void timeout(int sig)
{
	_exit(EXIT_FAILURE);
}

static int puppeteer(int fdin, int fdout, bool verbose)
{
	struct dialog *d = dialog;
	FILE *fpin, *fpout;
	int ret;

	/* Be lazy, use standard buffered I/O. */
	fpin = fdopen(fdin, "r");
	if (!fpin) {
		perror("fdopen");
		return EXIT_FAILURE;
	}

	fpout = fdopen(fdout, "w");
	if (!fpout) {
		perror("fdopen");
		return EXIT_FAILURE;
	}

	signal(SIGALRM, timeout);

	do {
		ret = consume_and_match(fpin, d->expect, verbose);
		if (ret) {
			error(0, EINVAL, "no input matched at step #%zd", d - dialog);
			return ret;
		}
		ret = send_next_command(fpout, d->send, verbose);
		if (ret) {
			error(0, EINVAL, "can't output command at step #%zd", d - dialog);
			return ret;
		}
		d++;
	} while (d->expect);

	return 0;
}

static void usage(void)
{
        fprintf(stderr, "usage: ptrace-sync [options]:\n");
        fprintf(stderr, "-v --verbose              display test<->gdb dialog\n");
}

#define short_optlist "v"

static const struct option options[] = {
	{
		.name = "verbose",
		.has_arg = no_argument,
		.val = 'v',
	},
	{ /* Sentinel */ }
};

int main(int argc, char *argv[])
{
	int ret, cldin[2], cldout[2], c;
	bool verbose = false;
	char me[PATH_MAX];
	ssize_t count;

	if (getenv("__EVL_DEBUGGEE__"))
		return debuggee();

	for (;;) {
		c = getopt_long(argc, argv, short_optlist, options, NULL);
		if (c == EOF)
			break;

		switch (c) {
		case 0:
			break;
		case 'v':
			verbose = true;
			break;
		case '?':
		default:
			usage();
			return 1;
		}
	}

	if (optind < argc) {
		usage();
		return 1;
	}

	count = readlink("/proc/self/exe", me, sizeof(me) - 1);
	if (count < 0) {
		perror("readlink");
		return EXIT_FAILURE;
	}
	me[count] = '\0';

	ret = pipe(cldin);
	if (ret) {
		perror("pipe");
		return EXIT_FAILURE;
	}

	ret = pipe(cldout);
	if (ret) {
		perror("pipe");
		return EXIT_FAILURE;
	}

	switch (fork()) {
	case 0:
		dup2(cldin[0], 0);
		dup2(cldout[1], 1);
		dup2(cldout[1], 2);
		execlp("gdb", "gdb", "--nx", me, NULL);
		perror("exec");
		return EXIT_FAILURE;
	case -1:
		perror("fork");
		return EXIT_FAILURE;
	default:
		return puppeteer(cldout[0], cldin[1], verbose);
	}
}
