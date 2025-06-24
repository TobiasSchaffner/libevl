 /*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <error.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/gpio.h>
#include <evl/devices/gpio-abi.h>
#include <evl/evl.h>
#include <latmon.h>
#include "latmus.h"
#include "stats.h"
#include "timer.h"
#include "gpio.h"

#define LATMON_TIMEOUT_SECS  5

static int gpio_infd = -1, gpio_outfd = -1;

static int gpio_inpin, gpio_outpin;

static int gpio_hdinflags = GPIOHANDLE_REQUEST_INPUT,
	gpio_hdoutflags = GPIOHANDLE_REQUEST_OUTPUT;

static int gpio_evinflags;

static int lat_sock = -1;

static struct in_addr gpio_monitor_ip;

static pthread_t logger;

static sem_t logger_done;

static int parse_gpio_spec(const char *spec, int *pin,
		int *hdflags, int *evflags)
{
	char *s, *p, *endptr, *devname;
	int fd, ret;

	s = strdup(spec);
	p = strtok(s, ",");
	if (p == NULL)
		error(1, EINVAL, "no GPIO device in spec: %s", spec);

	ret = asprintf(&devname, "/dev/%s", p);
	if (ret < 0)
		error(1, ENOMEM, "asprintf()");

	p = strtok(NULL, ",");
	if (p == NULL)
		error(1, EINVAL, "no GPIO pin in spec: %s", spec);

	*pin = (int)strtol(p, &endptr, 10);
	if (*pin < 0 || endptr == p)
		error(1, EINVAL, "invalid GPIO pin number in spec: %s",
			spec);

	p = strtok(NULL, ",");
	if (evflags) {
		if (p) {
			if (!strcmp(p, "rising-edge"))
				*evflags = GPIOEVENT_REQUEST_RISING_EDGE;
			else if  (!strcmp(p, "falling-edge"))
				*evflags = GPIOEVENT_REQUEST_FALLING_EDGE;
			else
				error(1, EINVAL, "invalid edge type in spec: %s",
					spec);
		} else	/* Default is rising edge. */
			*evflags = GPIOEVENT_REQUEST_RISING_EDGE;
	} else if (p)
		error(1, EINVAL, "trailing garbage in spec: %s",
			spec);

	fd = open(devname, O_RDONLY);
	if (fd < 0)
		error(1, errno, "open(%s)", devname);

	free(devname);
	free(s);

	return fd;
}

static void listen_broadcast_address(struct sockaddr_in *peer)
{
	struct sockaddr_in local;
	socklen_t socklen;
	int sock;
	char c;

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		error(1, errno,"failed to create socket");

	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_port = htons(LATMON_NET_PORT);
	local.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0)
		error(1, errno, "failed to bind socket");

	/*
	 * We don't actually care about the received message,
	 * we only need the sender's address.
	 */
	socklen = sizeof(*peer);
	while (recvfrom(sock, &c, 1, 0, (struct sockaddr *)peer, &socklen) < 0) {
		if (errno != EINTR)
			error(1, errno,
				"failed to receive broadcast message");
	}

	close(sock);
}

void find_latmon_ip(const char *host)
{
	struct sockaddr_in peer;
	int ret;

	if (!strcmp(host, "broadcast")) {
		listen_broadcast_address(&peer);
		gpio_monitor_ip = peer.sin_addr;
	} else {
		ret = find_host_ip(host, &gpio_monitor_ip);
		if (ret)
			error(1, -ret, "getaddrinfo(%s)", host);
	}
}
static void setup_gpio_pins(int *fds)
{
	struct gpiohandle_request out;
	struct gpioevent_request in;
	int ret;

	in.handleflags = gpio_hdinflags;
	in.eventflags = gpio_evinflags;
	in.lineoffset = gpio_inpin;
	strcpy(in.consumer_label, "latmon-pulse");

	ret = ioctl(gpio_infd, GPIO_GET_LINEEVENT_IOCTL, &in);
	if (ret)
		error(1, errno, "ioctl(GPIO_GET_LINEEVENT_IOCTL)");

	out.lineoffsets[0] = gpio_outpin;
        out.lines = 1;
	out.flags = gpio_hdoutflags;
        out.default_values[0] = 1;
	strcpy(out.consumer_label, "latmon-ack");

	ret = ioctl(gpio_outfd, GPIO_GET_LINEHANDLE_IOCTL, &out);
	if (ret)
		error(1, errno, "ioctl(GPIO_GET_LINEHANDLE_IOCTL)");

	fds[0] = in.fd;
	fds[1] = out.fd;
}

static void *gpio_responder_thread(void *arg)
{
	struct gpiohandle_data data = { 0 };
	struct gpioevent_data event;
	typeof(ioctl) *do_ioctl;
	typeof(read) *do_read;
	const int ackval = 0;	/* Remote observes falling edges. */
	int fds[2], efd, ret;

	setup_gpio_pins(fds);

	if (test_gpiolat == OOB_MODE) {
		efd = evl_attach_self("/gpio-responder:%d", getpid());
		if (efd < 0)
			error(1, -efd, "evl_attach_self() failed");

		ret = evl_set_thread_mode(efd, EVL_T_WOSS, NULL);
		if (ret)
			error(1, -ret, "evl_set_thread_mode(EVL_T_WOSS) failed");

		do_ioctl = oob_ioctl;
		do_read = oob_read;
	} else {
		do_ioctl = ioctl;
		do_read = read;
	}

	for (;;) {
		data.values[0] = !ackval;
		ret = do_ioctl(fds[1], GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
		if (ret)
			error(1, errno,
			"ioctl(GPIOHANDLE_SET_LINE_VALUES_IOCTL) failed");

		ret = do_read(fds[0], &event, sizeof(event));
		if (ret != sizeof(event))
			break;

		data.values[0] = ackval;
		ret = do_ioctl(fds[1], GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
		if (ret)
			error(1, errno,
				"ioctl(GPIOHANDLE_SET_LINE_VALUES_IOCTL) failed");
	}

	return NULL;
}

static ssize_t read_net_data(void *buf, size_t len)
{
	ssize_t count = 0, ret;
	struct pollfd pollfd;

	pollfd.fd = lat_sock;
	pollfd.events = POLLIN;
	pollfd.revents = 0;

	do {
		/* Make sure to detect latmon unresponsivess. */
		ret = poll(&pollfd, 1, LATMON_TIMEOUT_SECS * 1000);
		if (ret <= 0)
			return -ETIMEDOUT;
		ret = recv(lat_sock, buf + count, len - count, 0);
		if (ret <= 0)
			return ret;
		count += ret;
	} while (count < (ssize_t)len);

	return count;
}

struct sock_logger_arg {
	struct statistics *st;
	bool hung;
};

static void *sock_logger_thread(void *arg)
{
	struct sock_logger_arg *larg = arg;
	struct statistics *st = larg->st;
	struct latmus_measurement meas;
	struct latmon_net_data ndata;
	ssize_t ret, round = 0;
	size_t cell;

	for (;;) {
		ret = read_net_data(&ndata, sizeof(ndata));
		if (ret <= 0)
			goto unresponsive;

		/*
		 * Receiving an empty data record means that we got
		 * the trailing data in the previous round, so
		 * we are done with sample bulks now.
		 */
		if (ndata.samples == 0)
			break;

		/* This is valid sample data, log it. */
		meas.sum_lat = ((__s64)ntohl(ndata.sum_lat_hi)) << 32 |
			ntohl(ndata.sum_lat_lo);
		meas.min_lat = ntohl(ndata.min_lat);
		meas.max_lat = ntohl(ndata.max_lat);
		meas.overruns = ntohl(ndata.overruns);
		meas.samples = ntohl(ndata.samples);
		log_results(st, &meas, round++);
	}

	if (!st->histogram)
		goto out;

	ret = read_net_data(st->histogram, st->h_cells * sizeof(int32_t));
	if (ret <= 0) {
	unresponsive:
		larg->hung = true;
		kill(getpid(), SIGHUP);
	} else {
		for (cell = 0; cell < st->h_cells; cell++)
			st->histogram[cell] = ntohl(st->histogram[cell]);
	}
out:
	sem_post(&logger_done);

	return NULL;
}

void run_gpio_test(bool oob_mode, size_t histogram_cells)
{
	struct statistics statistics = {
		.ops = {
			.more_data = more_timer_data,
			.wrap_data_page = wrap_timer_data_page,
			.print_summary = print_timer_summary,
			.get_elapsed_secs = get_timer_elapsed_secs,
		},
	};
	struct latmon_net_request req;
	struct sock_logger_arg larg;
	struct sockaddr_in in_addr;
	struct timespec timeout;
	pthread_t responder;
	time_t duration;
	int ret, sig;

	sem_init(&logger_done, 0, 0);

	gpio_infd = parse_gpio_spec(optarg, &gpio_inpin,
				&gpio_hdinflags, &gpio_evinflags);

	gpio_outfd = parse_gpio_spec(optarg, &gpio_outpin,
				&gpio_hdoutflags, NULL);

	init_statistics("gpio", &statistics, histogram_cells);

	find_latmon_ip(peer_host);

	if (oob_mode) {
		ret = evl_init();
		if (ret)
			error(1, -ret, "evl_init()");
		gpio_hdinflags |= GPIOHANDLE_REQUEST_OOB;
		gpio_hdoutflags |= GPIOHANDLE_REQUEST_OOB;
	}

	lat_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (lat_sock < 0)
		error(1, errno, "socket()");

	if (verbosity)
		printf("connecting to latmon at %s:%d...\n",
			inet_ntoa(gpio_monitor_ip), LATMON_NET_PORT);

	memset(&in_addr, 0, sizeof(in_addr));
	in_addr.sin_family = AF_INET;
	in_addr.sin_addr = gpio_monitor_ip;
	in_addr.sin_port = htons(LATMON_NET_PORT);
	ret = connect(lat_sock, (struct sockaddr *)&in_addr,
		sizeof(in_addr));
	if (ret)
		error(1, errno, "connect()");

	if (verbosity && test_gpiolat == INBAND_MODE)
		printf("CAUTION: measuring in-band response time (no EVL there)\n");

	create_responder(&responder, responder_priority, gpio_responder_thread);
	larg.st = &statistics;
	larg.hung = false;
	create_logger(&logger, sock_logger_thread, &larg);

	notify_start(1); /* +1 warm-up time */
	req.period_usecs = htonl(period_usecs); /* Non-zero, means start. */
	req.histogram_cells = htonl(statistics.h_cells);
	ret = send(lat_sock, &req, sizeof(req), 0);
	if (ret != sizeof(req))
		error(1, errno, "send() start");

	sigwait(&sigmask, &sig);

	/*
	 * From now on, we may wait up to LATMON_TIMEOUT_SECS
	 * max. between messages from the remote latency monitor
	 * before declaring it unresponsive.
	 */
	if (!larg.hung) {
		req.period_usecs = 0; /* Zero means stop. */
		req.histogram_cells = 0;
		ret = send(lat_sock, &req, sizeof(req), 0);
		if (ret != sizeof(req)) {
			larg.hung = true;
		} else {
			clock_gettime(CLOCK_REALTIME, &timeout);
			timeout.tv_sec += LATMON_TIMEOUT_SECS;
			if (sem_timedwait(&logger_done, &timeout))
				larg.hung = true;
		}
	}

	pthread_cancel(responder);
	pthread_join(responder, NULL);
	pthread_cancel(logger);

	duration = time(NULL) - start_time - 1; /* -1s warm-up time */
	consume_statistics(&statistics, 1, duration, spurious_inband_switches > 0);

	if (larg.hung)
		error(1, ETIMEDOUT, "latmon at %s is unresponsive",
			inet_ntoa(gpio_monitor_ip));

	close(lat_sock);
}
