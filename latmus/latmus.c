/*
 * SPDX-License-Identifier: MIT
 *
 * Derived from Xenomai Cobalt's latency & autotune utilities
 * (http://git.xenomai.org/xenomai-3.git/)
 * Copyright (C) 2014 Gilles Chanteperdrix <gch@xenomai.org>
 * Copyright (C) 2018-2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdbool.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <error.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <evl/evl.h>
#include <evl/signal.h>
#include "timer.h"
#include "gpio.h"
#include "tuning.h"

#define OOB_GPIO_LAT    1
#define INBAND_GPIO_LAT 2

static int test_irqlat, test_klat,
	test_ulat, test_sirqlat,
	test_gpiolat;

static bool reset, background;

static bool force_cpu;

#define short_optlist "ikusrqbKmtp:A:T:v::l:g::H:P:c:Z:z:I:O:C:"

static const struct option options[] = {
	{
		.name = "irq",
		.has_arg = no_argument,
		.val = 'i'
	},
	{
		.name = "kernel",
		.has_arg = no_argument,
		.val = 'k'
	},
	{
		.name = "user",
		.has_arg = no_argument,
		.val = 'u'
	},
	{
		.name = "sirq",
		.has_arg = no_argument,
		.val = 's'
	},
	{
		.name = "reset",
		.has_arg = no_argument,
		.val = 'r'
	},
	{
		.name = "quiet",
		.has_arg = no_argument,
		.val = 'q'
	},
	{
		.name = "background",
		.has_arg = no_argument,
		.val = 'b'
	},
	{
		.name = "keep-going",
		.has_arg = no_argument,
		.val = 'K'
	},
	{
		.name = "measure",
		.has_arg = no_argument,
		.val = 'm',
	},
	{
		.name = "tune",
		.has_arg = no_argument,
		.val = 't',
	},
	{
		.name = "period",
		.has_arg = required_argument,
		.val = 'p',
	},
	{
		.name = "timeout",
		.has_arg = required_argument,
		.val = 'T',
	},
	{
		.name = "maxlat-abort",
		.has_arg = required_argument,
		.val = 'A',
	},
	{
		.name = "verbose",
		.has_arg = optional_argument,
		.val = 'v',
	},
	{
		.name = "lines",
		.has_arg = required_argument,
		.val = 'l',
	},
	{
		.name = "plot",
		.has_arg = optional_argument,
		.val = 'g',
	},
	{
		.name = "histogram",
		.has_arg = required_argument,
		.val = 'H',
	},
	{
		.name = "priority",
		.has_arg = required_argument,
		.val = 'P',
	},
	{
		.name = "cpu",
		.has_arg = required_argument,
		.val = 'c',
	},
	{
		.name = "force-cpu",
		.has_arg = required_argument,
		.val = 'C',
	},
	{
		.name = "oob-gpio",
		.has_arg = required_argument,
		.val = 'Z',
	},
	{
		.name = "inband-gpio",
		.has_arg = required_argument,
		.val = 'z',
	},
	{
		.name = "gpio-in",
		.has_arg = required_argument,
		.val = 'I',
	},
	{
		.name = "gpio-out",
		.has_arg = required_argument,
		.val = 'O',
	},
	{ /* Sentinel */ }
};

static void sigdebug_handler(int sig, siginfo_t *si, void *context)
{
	if (sigdebug_marked(si)) {
		switch (sigdebug_cause(si)) {
		case EVL_HMDIAG_SIGDEMOTE:
		case EVL_HMDIAG_SYSDEMOTE:
		case EVL_HMDIAG_EXDEMOTE:
		case EVL_HMDIAG_LKDEPEND:
			spurious_inband_switches++;
			if (abort_on_switch)
				kill(getpid(), SIGHUP);
			break;
		case EVL_HMDIAG_WATCHDOG:
		case EVL_HMDIAG_LKIMBALANCE:
		case EVL_HMDIAG_LKSLEEP:
		default:
			exit(99);
		}
	}
}

static void set_cpu_affinity(void)
{
	cpu_set_t cpu_set;
	int ret;

	CPU_ZERO(&cpu_set);
	CPU_SET(responder_cpu, &cpu_set);
	ret = sched_setaffinity(0, sizeof(cpu_set), &cpu_set);
	if (ret)
		error(1, errno, "cannot set affinity to CPU%d",
		      responder_cpu);
}

static void restrict_c_state(void)
{
	__s32 val = 0;
	int fd;

	fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (fd < 0)
		return;

	if (write(fd, &val, sizeof(val) == sizeof(val)))
		c_state_restricted = true;
}

static void parse_cpu_list(const char *path, cpu_set_t *cpuset)
{
	char *p, *range, *range_p = NULL, *id, *id_r;
	int start, end, cpu;
	char buf[BUFSIZ];
	FILE *fp;

	CPU_ZERO(cpuset);

	fp = fopen(path, "r");
	if (fp == NULL)
		return;

	if (!fgets(buf, sizeof(buf), fp))
		goto out;

	p = buf;
	while ((range = strtok_r(p, ",", &range_p)) != NULL) {
		if (*range == '\0' || *range == '\n')
			goto next;
		end = -1;
		id = strtok_r(range, "-", &id_r);
		if (id) {
			start = atoi(id);
			id = strtok_r(NULL, "-", &id_r);
			if (id)
				end = atoi(id);
			else if (end < 0)
				end = start;
			for (cpu = start; cpu <= end; cpu++)
				CPU_SET(cpu, cpuset);
		}
	next:
		p = NULL;
	}
out:
	fclose(fp);
}

static void determine_responder_cpu(bool inband_test)
{
	cpu_set_t oob_cpus, best_cpus;
	int cpu;

	parse_cpu_list(ISOLATED_CPU_LIST, &isolated_cpus);
	parse_cpu_list(OOB_CPU_LIST, &oob_cpus);

	if (responder_cpu >= 0) {
		if (!inband_test && !CPU_ISSET(responder_cpu, &oob_cpus)) {
			if (verbosity)
				printf("CPU%d is not OOB-capable, "
					"picking a better one\n",
					responder_cpu);
			goto pick_oob;
		}
		goto finish;
	}

	if (force_cpu)
		goto finish;

	if (inband_test)
		goto pick_isolated;

	/*
	 * Pick a default CPU among the ones which are both
	 * OOB-capable and isolated. If EVL is not enabled, oob_cpus
	 * is empty so there is no best choice.
	 */
	CPU_AND(&best_cpus, &isolated_cpus, &oob_cpus);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &best_cpus)) {
			responder_cpu = cpu;
			goto finish;
		}
	}

	/*
	 * If no best choice, pick the first OOB-capable CPU we can
	 * find (if any).
	 */
pick_oob:
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &oob_cpus)) {
			responder_cpu = cpu;
			goto finish;
		}
	}

pick_isolated:
	/*
	 * This must be a kernel with no EVL support or we
	 * specifically need an isolated CPU.
	 */
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &isolated_cpus)) {
			responder_cpu = cpu;
			goto finish;
		}
	}

	/* Out of luck, run on the current CPU. */
	if (responder_cpu < 0)
		responder_cpu = sched_getcpu();
finish:
	if (CPU_ISSET(responder_cpu, &isolated_cpus))
		responder_cpu_state = EVL_CPU_ISOL;
}

static void usage(void)
{
        fprintf(stderr, "usage: latmus [options]:\n");
        fprintf(stderr, "-m --measure            measure latency on timer event [default]\n");
        fprintf(stderr, "-t --tune               tune the EVL core timer\n");
        fprintf(stderr, "-i --irq                measure/tune interrupt latency\n");
        fprintf(stderr, "-k --kernel             measure/tune kernel scheduling latency\n");
        fprintf(stderr, "-u --user               measure/tune user scheduling latency\n");
        fprintf(stderr, "    [ if none of --irq, --kernel or --user is given,\n"
                        "      tune for all contexts ]\n");
        fprintf(stderr, "-s --sirq               measure in-band response time to synthetic irq\n");
        fprintf(stderr, "-p --period=<us>        sampling period\n");
        fprintf(stderr, "-P --priority=<prio>    responder thread priority [=90]\n");
        fprintf(stderr, "-c --cpu=<n>            pin responder thread to CPU [=current]\n");
        fprintf(stderr, "-C --force-cpu=<n>      similar to -c, accept non-isolated CPU\n");
        fprintf(stderr, "-r --reset              reset core timer gravity to factory default\n");
        fprintf(stderr, "-b --background         run in the background (daemon mode)\n");
        fprintf(stderr, "-K --keep-going         keep going on unexpected switch to in-band mode\n");
        fprintf(stderr, "-A --max-abort=<us>     abort if maximum latency exceeds threshold\n");
        fprintf(stderr, "-T --timeout=<t>[dhms]  stop measurement after <t> [d(ays)|h(ours)|m(inutes)|s(econds)]\n");
        fprintf(stderr, "-v --verbose[=level]    set verbosity level [=1]\n");
        fprintf(stderr, "-q --quiet              quiet mode (i.e. --verbose=0)\n");
        fprintf(stderr, "-l --lines=<num>        result lines per page, 0 = no pagination [=21]\n");
        fprintf(stderr, "-H --histogram[=<nr>]   set histogram size to <nr> cells [=200]\n");
        fprintf(stderr, "-g --plot=<filename>    dump histogram data to file (gnuplot format)\n");
        fprintf(stderr, "-Z --oob-gpio=<host>    measure EVL response time to GPIO event via <host|broadcast>\n");
        fprintf(stderr, "-z --inband-gpio=<host> measure in-band response time to GPIO event via <host|broadcast>\n");
        fprintf(stderr, "-I --gpio-in=<spec>     input GPIO line configuration\n");
        fprintf(stderr, "   with <spec> = gpiochip-devname,pin-number[,rising-edge|falling-edge]\n");
        fprintf(stderr, "-O --gpio-out=<spec>    output GPIO line configuration\n");
        fprintf(stderr, "   with <spec> = gpiochip-devname,pin-number\n");
}

static void bad_usage(int argc, char *const argv[])
{
	int last = optind < argc ? optind : argc - 1;
	printf("** Uh, you lost me somewhere near '%s' (arg #%d)\n", argv[last], last);
	usage();
}

int main(int argc, char *const argv[])
{
	int ret, c, spec, type, max_prio, lindex;
	const char *plot_filename = NULL;
	struct sigaction sa;
	bool tuning = false;
	char *endptr;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv, short_optlist, options, &lindex);
		if (c == EOF)
			break;

		switch (c) {
		case 0:
			break;
		case 'i':
			test_irqlat = 1;
			break;
		case 'k':
			test_klat = 1;
			break;
		case 'u':
			test_ulat = 1;
			break;
		case 's':
			test_sirqlat = 1;
			break;
		case 'r':
			reset = true;
			break;
		case 'q':
			verbosity = 0;
			break;
		case 'b':
			background = true;
			break;
		case 'K':
			abort_on_switch = false;
			break;
		case 'm':
			tuning = false;
			break;
		case 't':
			tuning = true;
			break;
		case 'p':
			period_usecs = atoi(optarg);
			if (period_usecs <= 0 || period_usecs > 1000000)
				error(1, EINVAL, "invalid sampling period "
				      "(0 < period < 1000000)");
			break;
		case 'A':
			abort_threshold = atoi(optarg) * 1000; /* ns */
			if (abort_threshold <= 0)
				error(1, EINVAL, "invalid timeout");
			break;
		case 'T':
			timeout = (int)strtol(optarg, &endptr, 10);
			if (timeout < 0 || endptr == optarg)
				error(1, EINVAL, "invalid timeout");
			switch (*endptr) {
			case 'd':
				timeout *= 24;
				__fallthrough;
			case 'h':
				timeout *= 60;
				__fallthrough;
			case 'm':
				timeout *= 60;
				break;
			case 's':
			case '\0':
				break;
			default:
				error(1, EINVAL, "invalid time modifier: '%c'",
					*endptr);
			}
			break;
		case 'v':
			verbosity = optarg ? atoi(optarg) : 1;
			break;
		case 'l':
			data_lines = atoi(optarg);
			break;
		case 'g':
			if (optarg && strcmp(optarg, "-"))
				plot_filename = optarg;
			else
				plot_fp = stdout;
			break;
		case 'H':
			histogram_cells = atoi(optarg);
			if (histogram_cells < 1 || histogram_cells > 1000)
				error(1, EINVAL, "invalid number of histogram cells "
				      "(0 < cells <= 1000)");
			break;
		case 'P':
			max_prio = sched_get_priority_max(SCHED_FIFO);
			responder_priority = atoi(optarg);
			if (responder_priority < 0 || responder_priority > max_prio)
				error(1, EINVAL, "invalid thread priority "
				      "(0 < priority < %d)", max_prio);
			break;
		case 'C':
			force_cpu = true;
			__fallthrough;
		case 'c':
			responder_cpu = atoi(optarg);
			if (responder_cpu < 0 || responder_cpu >= CPU_SETSIZE)
				error(1, EINVAL, "invalid CPU number");
			break;
		case 'z':
		case 'Z':
			test_gpiolat = (c == 'z') + 1;
			find_latmon_ip(optarg);
			break;
		case 'I':
			gpio_infd = parse_gpio_spec(optarg, &gpio_inpin,
					&gpio_hdinflags, &gpio_evinflags);
			break;
		case 'O':
			gpio_outfd = parse_gpio_spec(optarg, &gpio_outpin,
					&gpio_hdoutflags, NULL);
			break;
		case '?':
		default:
			bad_usage(argc, argv);
			return 1;
		}
	}

	if (optind < argc) {
		bad_usage(argc, argv);
		return 1;
	}

	determine_responder_cpu(test_gpiolat == INBAND_GPIO_LAT);

	setlinebuf(stdout);
	setlinebuf(stderr);

	if (!tuning && !timeout && !verbosity) {
		fprintf(stderr, "--quiet requires --timeout, ignoring --quiet\n");
		verbosity = 1;
	}

	if (background && verbosity) {
		fprintf(stderr, "--background requires --quiet, taming verbosity down\n");
		verbosity = 0;
	}

	if (tuning && (plot_filename || plot_fp)) {
		fprintf(stderr, "--plot implies --measure, ignoring --plot\n");
		plot_filename = NULL;
		plot_fp = NULL;
	}

	if (background) {
		signal(SIGHUP, SIG_IGN);
		ret = daemon(0, 0);
		if (ret)
			error(1, errno, "cannot daemonize");
	}

	set_cpu_affinity();
	restrict_c_state();

	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);
	sigaddset(&sigmask, SIGHUP);
	sigaddset(&sigmask, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &sigmask, NULL);

	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = sigdebug_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGDEBUG, &sa, NULL);

	spec = test_irqlat || test_klat || test_ulat || test_sirqlat || test_gpiolat;
	if (!tuning) {
		if (!spec)
			test_ulat = 1;
		else if (test_irqlat + test_klat + test_ulat + test_sirqlat +
			(!!test_gpiolat) > 1)
			error(1, EINVAL, "only one of -u, -k, -i, -s, -z or -Z "
			      "in measurement mode");
	} else {
		/* Default to tune for all contexts. */
		if (!spec)
			test_irqlat = test_klat = test_ulat = 1;
		else if (test_sirqlat || test_gpiolat)
			error(1, EINVAL, "-s/-z and -t are mutually exclusive");
	}

	if (test_gpiolat != INBAND_GPIO_LAT) {
		ret = evl_init();
		if (ret)
			error(1, -ret, "evl_init()");
	}

	if (!test_gpiolat) {
		latmus_fd = open("/dev/latmus", O_RDWR);
		if (latmus_fd < 0)
			error(1, errno, "cannot open latmus device");

		if (reset) {
			ret = ioctl(latmus_fd, EVL_LATIOC_RESET);
			if (ret)
				error(1, errno, "reset failed");
		}
	} else {
		if (gpio_infd < 0 || gpio_outfd < 0)
			error(1, EINVAL, "-[zZ] require -I, -O for GPIO settings");
	}

	time(&start_time);

	if (!tuning) {
		if (plot_filename) {
			if (!access(plot_filename, F_OK))
				error(1, EINVAL, "declining to overwrite %s",
				      plot_filename);
			plot_fp = fopen(plot_filename, "w");
			if (plot_fp == NULL)
				error(1, errno, "cannot open %s for writing",
				      plot_filename);
		}
		type = test_irqlat ? EVL_LAT_IRQ : test_klat ?
			EVL_LAT_KERN : test_sirqlat ? EVL_LAT_SIRQ :
			test_ulat ? EVL_LAT_USER :
			EVL_LAT_LAST + test_gpiolat;
		do_measurement(type, !(test_irqlat || test_sirqlat),
			test_gpiolat == OOB_GPIO_LAT);
	} else {
		if (verbosity)
			printf("== latmus started for core tuning, "
			       "period=%d microseconds (may take a while)\n",
			       period_usecs);

		ret = evl_attach_self("/clock-tuner:%d", getpid());
		if (ret < 0)
			error(1, -ret, "evl_attach_self() failed");

		if (test_irqlat)
			do_tuning(EVL_LAT_IRQ);

		if (test_klat)
			do_tuning(EVL_LAT_KERN);

		if (test_ulat)
			do_tuning(EVL_LAT_USER);

		if (verbosity)
			printf("== tuning completed after %ds\n",
			       (int)(time(NULL) - start_time));
	}

	return 0;
}
