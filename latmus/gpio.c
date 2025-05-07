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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <linux/gpio.h>
#include <evl/devices/gpio.h>
#include <evl/thread.h>
#include <latmon.h>
#include "gpio.h"

#define LATMON_TIMEOUT_SECS  5

int gpio_infd = -1, gpio_outfd = -1;

int gpio_inpin, gpio_outpin;

int gpio_hdinflags = GPIOHANDLE_REQUEST_INPUT,
	gpio_hdoutflags = GPIOHANDLE_REQUEST_OUTPUT;

int gpio_evinflags;

static int lat_sock = -1;

static struct in_addr gpio_monitor_ip;

void setup_gpio_pins(int *fds)
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

void *gpio_responder_thread(void *arg)
{
	struct gpiohandle_data data = { 0 };
	struct gpioevent_data event;
	typeof(ioctl) *do_ioctl;
	typeof(read) *do_read;
	const int ackval = 0;	/* Remote observes falling edges. */
	int fds[2], efd, ret;

	setup_gpio_pins(fds);

	if (context_type == EVL_LAT_OOB_GPIO) {
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

static void *sock_logger_thread(void *arg)
{
	struct latmus_measurement meas;
	struct latmon_net_data ndata;
	bool *no_response = arg;
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
		log_results(&meas, round++);
	}

	if (histogram == NULL)
		goto out;

	ret = read_net_data(histogram, histogram_cells * sizeof(int32_t));
	if (ret <= 0) {
	unresponsive:
		*no_response = true;
		kill(getpid(), SIGHUP);
	} else {
		for (cell = 0; cell < histogram_cells; cell++)
			histogram[cell] = ntohl(histogram[cell]);
	}
out:
	sem_post(&logger_done);

	return NULL;
}

void setup_measurement_on_gpio(bool oob_mode)
{
	struct latmon_net_request req;
	struct sockaddr_in in_addr;
	bool latmon_hung = false;
	struct timespec timeout;
	int ret, sig;

	if (oob_mode) {
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

	if (verbosity && context_type == EVL_LAT_INBAND_GPIO)
		printf("CAUTION: measuring in-band response time (no EVL there)\n");

	create_responder(&responder, responder_priority, gpio_responder_thread);
	create_logger(&logger, sock_logger_thread, &latmon_hung);

	notify_start();
	req.period_usecs = htonl(period_usecs); /* Non-zero, means start. */
	req.histogram_cells = histogram ? htonl(histogram_cells) : 0;
	ret = send(lat_sock, &req, sizeof(req), 0);
	if (ret != sizeof(req))
		error(1, errno, "send() start");

	sigwait(&sigmask, &sig);

	/*
	 * From now on, we may wait up to LATMON_TIMEOUT_SECS
	 * max. between messages from the remote latency monitor
	 * before declaring it unresponsive.
	 */
	if (!latmon_hung) {
		req.period_usecs = 0; /* Zero means stop. */
		req.histogram_cells = 0;
		ret = send(lat_sock, &req, sizeof(req), 0);
		if (ret != sizeof(req)) {
			error(1, errno, "send() stop");
			latmon_hung = true;
		} else {
			clock_gettime(CLOCK_REALTIME, &timeout);
			timeout.tv_sec += LATMON_TIMEOUT_SECS;
			if (sem_timedwait(&logger_done, &timeout))
				latmon_hung = true;
		}
	}

	if (latmon_hung)
		error(1, ETIMEDOUT, "latmon at %s is unresponsive",
			inet_ntoa(gpio_monitor_ip));

	close(lat_sock);
}

int parse_gpio_spec(const char *spec, int *pin,
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
	struct addrinfo hints, *res;
	struct sockaddr_in peer;
	int ret;

	if (!strcmp(host, "broadcast")) {
		listen_broadcast_address(&peer);
		gpio_monitor_ip = peer.sin_addr;
	} else {
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_ADDRCONFIG;

		ret = getaddrinfo(host, NULL, &hints, &res);
		if (ret)
			error(1, ret == EAI_SYSTEM ? errno : ESRCH,
				"getaddrinfo(%s)", host);

		gpio_monitor_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
	}
}
