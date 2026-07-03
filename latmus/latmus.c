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
#include <pthread.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <evl/evl.h>
#include <evl/signal-abi.h>
#include "latmus.h"
#include "stats.h"
#include "timer.h"
#include "gpio.h"
#include "net.h"
#include "tuning.h"

int test_irqlat = 0, test_klat = 0,
	test_ulat = 0, test_sirqlat = 0,
	test_gpiolat = 0, test_netlat = 0;

cpu_set_t isolated_cpus;

sigset_t sigmask;

int verbosity = 1,
	abort_threshold = 0;

time_t timeout = 0;

bool abort_on_switch = true,
	c_state_restricted = false;

int context_type = EVL_LAT_USER;

clockid_t reference_clock = CLOCK_MONOTONIC;

unsigned int spurious_inband_switches = 0;

time_t start_time = 0;

FILE *plot_fp = NULL;

int data_lines = 21;

size_t packet_size = 0;	/* Pick default. */

int responder_priority = -1;

int responder_cpu = -1;

int responder_cpu_state = 0;

unsigned int period_usecs = 1000; /* 1ms */

const char *peer_host = NULL;

const char *local_netif = NULL;

const char *context_labels[] = {
	[EVL_LAT_IRQ] = "irq",
	[EVL_LAT_SIRQ] = "sirq",
	[EVL_LAT_KERN] = "kernel",
	[EVL_LAT_USER] = "user",
	[EVL_LAT_OOB_GPIO] = "oob-gpio",
	[EVL_LAT_INBAND_GPIO] = "inband-gpio",
	[EVL_LAT_NET] = "net",
};

int latmus_fd = -1;

static bool reset, background;

static bool force_cpu;

#define short_optlist "ikusrqbKmtnp:A:T:v::l:g::H:P:c:Z:z:I:O:C:E:S:L:M::"

static const struct option options[] = {
	{
		.name = "irq",
		.has_arg = no_argument,
		.val = 'i',
	},
	{
		.name = "kernel",
		.has_arg = no_argument,
		.val = 'k',
	},
	{
		.name = "user",
		.has_arg = no_argument,
		.val = 'u',
	},
	{
		.name = "sirq",
		.has_arg = no_argument,
		.val = 's',
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
		.name = "net",
		.has_arg = required_argument,
		.val = 'E',
	},
	{
		.name = "packet-size",
		.has_arg = required_argument,
		.val = 'S'
	},
	{
		.name = "local-ip",
		.has_arg = required_argument,
		.val = 'L'
	},
	{
		.name = "no-check",
		.has_arg = no_argument,
		.val = 'n',
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
		.name = "clock-monotonic",
		.has_arg = optional_argument,
		.flag = &reference_clock,
		.val = CLOCK_MONOTONIC,
	},
	{
		.name = "clock-raw",
		.has_arg = optional_argument,
		.flag = &reference_clock,
		.val = CLOCK_MONOTONIC_RAW,
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

const char *get_refclock_name(void)
{
	if (reference_clock == CLOCK_MONOTONIC_RAW)
		return "raw monotonic";

	return "monotonic";
}

void notify_start(int delay)
{
	if (timeout)
		alarm(timeout + delay);
}

void create_responder(pthread_t *tid, int priority, void *(*responder)(void *))
{
	struct sched_param param;
	pthread_attr_t attr;
	int ret;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	param.sched_priority = priority;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	ret = pthread_create(tid, &attr, responder, NULL);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "sampling thread");
}

void create_logger(pthread_t *tid, void *(*logger)(void *), void *arg)
{
	struct sched_param param;
	pthread_attr_t attr;
	int ret;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
	param.sched_priority = 0;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	ret = pthread_create(tid, &attr, logger, arg);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "logger thread");
}

static void do_measurement(size_t histogram_cells, bool no_check)
{
	const char *cpu_s = "";

	/*
	 * One and only one test is set in measurement mode (checked
	 * while parsing options).
	 */
	context_type =
		test_irqlat ? EVL_LAT_IRQ :
		test_klat ? EVL_LAT_KERN :
		test_sirqlat ? EVL_LAT_SIRQ :
		test_ulat ? EVL_LAT_USER :
		test_gpiolat == OOB_MODE ? EVL_LAT_OOB_GPIO :
		test_gpiolat == INBAND_MODE ? EVL_LAT_INBAND_GPIO :
		EVL_LAT_NET;

	if (!(responder_cpu_state & EVL_CPU_ISOL))
		cpu_s = " (not isolated)";

	if (verbosity > 0)
		fprintf(stdout, "warming up on CPU%d%s...\n", responder_cpu, cpu_s);
	else
		fprintf(stdout, "running quietly for %ld seconds on CPU%d%s\n",
			(long)timeout, responder_cpu, cpu_s);

	switch (context_type) {
	case EVL_LAT_OOB_GPIO:
		run_gpio_test(true, histogram_cells);
		break;
	case EVL_LAT_INBAND_GPIO:
		run_gpio_test(false, histogram_cells);
		break;
	case EVL_LAT_NET:
		run_net_test(no_check, histogram_cells);
		break;
	default:
		run_timer_test(histogram_cells);
	}

	if (spurious_inband_switches > 0) {
		fprintf(stderr, "\n*** WARNING: unexpected switches to in-band mode detected,\n"
		       "             latency figures displayed are NOT reliable.\n"
		       "             Please submit a bug report upstream.\n");
		if (abort_on_switch) {
			abort_on_switch = false;
			fprintf(stderr, "-- aborting\n");
		}
	}
}

int find_host_ip(const char *host, struct in_addr *addr)
{
	struct addrinfo hints, *res;
	int ret;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;

	ret = getaddrinfo(host, NULL, &hints, &res);
	if (ret)
		return ret == EAI_SYSTEM ? -errno : -ESRCH;

	*addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;

	return 0;
}

int find_netif_ip(const char *netif, struct in_addr *addr)
{
	struct ifaddrs *ifaddrs, *ifa;
	int ret;

	ret = getifaddrs(&ifaddrs);
	if (ret)
		error(1, errno, "getifaddrs(%s)", netif);

	for (ifa = ifaddrs; ifa; ifa = ifa->ifa_next) {
               if (ifa->ifa_addr == NULL)
                   continue;

               if (ifa->ifa_addr->sa_family != AF_INET)
		       continue;

	       if (strcmp(ifa->ifa_name, netif))
		       continue;

	       *addr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
	       return 0;
	}

	return -EINVAL;
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
        fprintf(stderr, "-m --measure               measure latency on timer event [default]\n");
        fprintf(stderr, "-t --tune                  tune the EVL core timer\n");
        fprintf(stderr, "-i --irq                   measure/tune interrupt latency\n");
        fprintf(stderr, "-k --kernel                measure/tune kernel scheduling latency\n");
        fprintf(stderr, "-u --user                  measure/tune user scheduling latency\n");
        fprintf(stderr, "    [ if none of --irq, --kernel or --user is given,\n"
                        "      tune for all contexts ]\n");
        fprintf(stderr, "-s --sirq                  measure in-band response time to synthetic irq\n");
        fprintf(stderr, "-p --period=<us>           sampling period\n");
        fprintf(stderr, "-P --priority=<prio>       responder thread priority [=90]\n");
        fprintf(stderr, "-c --cpu=<n>               pin responder thread to CPU [=current]\n");
        fprintf(stderr, "-C --force-cpu=<n>         similar to -c, accept non-isolated CPU\n");
        fprintf(stderr, "-r --reset                 reset core timer gravity to factory default\n");
        fprintf(stderr, "-b --background            run in the background (daemon mode)\n");
        fprintf(stderr, "-K --keep-going            keep going on unexpected switch to in-band mode\n");
        fprintf(stderr, "-A --max-abort=<us>        abort if maximum latency exceeds threshold\n");
        fprintf(stderr, "-T --timeout=<t>[dhms]     stop measurement after <t> [d(ays)|h(ours)|m(inutes)|s(econds)]\n");
        fprintf(stderr, "-v --verbose[=level]       set verbosity level [=1]\n");
        fprintf(stderr, "-q --quiet                 quiet mode (i.e. --verbose=0)\n");
        fprintf(stderr, "-l --lines=<num>           result lines per page, 0 = no pagination [=21]\n");
        fprintf(stderr, "-H --histogram[=<nr>]      set histogram size to <nr> cells [=200]\n");
        fprintf(stderr, "-g --plot=<filename>       dump histogram data to file (gnuplot format)\n");
        fprintf(stderr, "-z --inband-gpio=<host>    measure in-band response time to GPIO event via <host|'broadcast'>\n");
        fprintf(stderr, "-Z --oob-gpio=<host>       measure EVL response time to GPIO event via <host|'broadcast'>\n");
        fprintf(stderr, "-I --gpio-in=<spec>        input GPIO line configuration\n");
        fprintf(stderr, "   with <spec> = gpiochip-devname,pin-number[,rising-edge|falling-edge]\n");
        fprintf(stderr, "-O --gpio-out=<spec>       output GPIO line configuration\n");
        fprintf(stderr, "   with <spec> = gpiochip-devname,pin-number\n");
        fprintf(stderr, "-E --net=<host>            measure out-of-band UDP delay talking to <host>\n");
        fprintf(stderr, "   -L --local-if=<netif>   use specified local network interface\n");
        fprintf(stderr, "   -n --no-check           disable packet sequence check\n");
        fprintf(stderr, "   -S --packet-size=<n>    set the UDP packet size (> 20 bytes)\n");
        fprintf(stderr, "-M[m] --clock-monotonic    use CLOCK_MONOTONIC for tuning and measurements\n");
        fprintf(stderr, "-Mr   --clock-raw          use CLOCK_MONOTONIC_RAW for tuning and measurements\n");
}

static void bad_usage(int argc, char *const argv[])
{
	int last = optind < argc ? optind : argc - 1;
	printf("** Uh, you lost me somewhere near '%s' (arg #%d)\n", argv[last], last);
	usage();
}

int main(int argc, char *const argv[])
{
	const char *gpio_i_specs = NULL, *gpio_o_specs = NULL;
	const char *plot_filename = NULL;
	bool tuning = false, no_check = false;
	int ret, c, spec, max_prio, lindex;
	size_t histogram_cells = 0;
	struct sigaction sa;
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
		case 'z':
		case 'Z':
			test_netlat = (c == 'Z' ? OOB_MODE : INBAND_MODE);
			peer_host = optarg;
			break;
		case 'E':
			test_netlat = true;
			peer_host = optarg;
			break;
		case 'n':
			no_check = true;
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
		case 'I':
			gpio_i_specs = optarg;
			break;
		case 'O':
			gpio_o_specs = optarg;
			break;
		case 'L':
			local_netif = optarg;
			break;
		case 'M':
			if (optarg) {
				switch (*optarg) {
				case 'm':
					reference_clock = CLOCK_MONOTONIC;
					break;
				case 'r':
					reference_clock = CLOCK_MONOTONIC_RAW;
					break;
				default:
					error(1, EINVAL, "invalid clock modifier (expect [m]onotonic or [r]aw");
				}
			} else {
				reference_clock = CLOCK_MONOTONIC;
			}
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

	determine_responder_cpu(test_gpiolat == INBAND_MODE ||
				test_netlat == INBAND_MODE);

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

	if (plot_filename || plot_fp) {
		if (tuning) {
			fprintf(stderr, "--plot implies --measure only, ignoring --plot\n");
			plot_filename = NULL;
			plot_fp = NULL;
		} else {
			if (plot_filename) {
				if (!access(plot_filename, F_OK))
					error(1, EINVAL, "declining to overwrite %s",
						plot_filename);
				plot_fp = fopen(plot_filename, "w");
				if (!plot_fp)
					error(1, errno, "cannot open %s for writing",
						plot_filename);
			}
			if (histogram_cells == 0)
				histogram_cells = 200;
		}
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

	spec = test_irqlat || test_klat || test_ulat || test_sirqlat || test_gpiolat || test_netlat;
	if (!tuning) {
		if (!spec)
			test_ulat = 1;
		else if (test_irqlat + test_klat + test_ulat + test_sirqlat +
			(!!test_gpiolat) + (!!test_netlat) > 1)
			error(1, EINVAL, "only one of -u, -k, -i, -s, -[Zz] or -[Ee] "
			      "in measurement mode");
	} else {
		/* Default is to tune for all timer contexts. */
		if (!spec)
			test_irqlat = test_klat = test_ulat = 1;
		else if (test_sirqlat || test_gpiolat || test_netlat)
			error(1, EINVAL, "-s/-[Zz]/-[Ee] and -t are mutually exclusive");
	}

	if (!(test_gpiolat || test_netlat)) {
		latmus_fd = open("/dev/latmus", O_RDWR);
		if (latmus_fd < 0)
			error(1, errno, "cannot open latmus device");

		if (reset) {
			ret = ioctl(latmus_fd, EVL_LATIOC_RESET, (long)reference_clock);
			if (ret)
				error(1, errno, "reset failed");
		}
	} else {
		if (test_gpiolat && (!gpio_i_specs || !gpio_o_specs))
			error(1, EINVAL, "-[zZ] requires -I, -O for GPIO settings");

		if (test_netlat && !local_netif)
			error(1, EINVAL, "-[eE] requires -L<netif> to specify a local interface");
	}

	if (responder_priority < 0 && (tuning || !(test_irqlat || test_sirqlat)))
		responder_priority = test_netlat ? 10 : 98;

	time(&start_time);

	if (!tuning) {
		do_measurement(histogram_cells, no_check);
	} else {
		if (verbosity)
			printf("== latmus is now tuning the core timer (%s), "
			       "period=%d microseconds (may take a while)\n",
				get_refclock_name(), period_usecs);

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
