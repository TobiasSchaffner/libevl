 /*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <memory.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <evl/thread.h>
#include <evl/clock.h>
#include <evl/timer.h>
#include <evl/proxy.h>
#include <evl/net/net.h>
#include <evl/evl.h>
#include "latmus.h"
#include "stats.h"
#include "net.h"

#define LATMUS_NET_PORT  59059

#define FRAME_METADATA_LEN	20	/* ^xxxxxxxxxxxxxxxxx ... $NN */
#define DEFAULT_PACKET_LEN	64	/* payload=44 + FRAME_METADATA_LEN */

struct latmus_net_rx {
	char *packet;
	size_t last_serial;
	long invalid_lost;
	bool check_sequence;
	struct latmus_measurement queuing;
	struct latmus_measurement delivery;
};

struct latmus_net_tx {
	char *packet;
	struct sockaddr_in peer_in;
	struct latmus_measurement queuing;
	struct latmus_measurement delivery;
};

enum {	/* indices in statistics[] */
	RX_QUEUING,
	RX_DELIVERY,
	TX_QUEUING,
	TX_DELIVERY,
	NR_STATS,
};

struct latmus_net_desc {
	int s;
	size_t packet_size;
	struct latmus_net_tx tx;
	struct latmus_net_rx rx;
	struct statistics statistics[NR_STATS];
};

static unsigned int rounds_per_sec;

static const uint8_t crc8_ccitt[] = {
	0x00, 0x8d, 0x97, 0x1a, 0xa3, 0x2e, 0x34, 0xb9,
	0xcb, 0x46, 0x5c, 0xd1, 0x68, 0xe5, 0xff, 0x72,
	0x1b, 0x96, 0x8c, 0x01, 0xb8, 0x35, 0x2f, 0xa2,
	0xd0, 0x5d, 0x47, 0xca, 0x73, 0xfe, 0xe4, 0x69,
	0x36, 0xbb, 0xa1, 0x2c, 0x95, 0x18, 0x02, 0x8f,
	0xfd, 0x70, 0x6a, 0xe7, 0x5e, 0xd3, 0xc9, 0x44,
	0x2d, 0xa0, 0xba, 0x37, 0x8e, 0x03, 0x19, 0x94,
	0xe6, 0x6b, 0x71, 0xfc, 0x45, 0xc8, 0xd2, 0x5f,
	0x6c, 0xe1, 0xfb, 0x76, 0xcf, 0x42, 0x58, 0xd5,
	0xa7, 0x2a, 0x30, 0xbd, 0x04, 0x89, 0x93, 0x1e,
	0x77, 0xfa, 0xe0, 0x6d, 0xd4, 0x59, 0x43, 0xce,
	0xbc, 0x31, 0x2b, 0xa6, 0x1f, 0x92, 0x88, 0x05,
	0x5a, 0xd7, 0xcd, 0x40, 0xf9, 0x74, 0x6e, 0xe3,
	0x91, 0x1c, 0x06, 0x8b, 0x32, 0xbf, 0xa5, 0x28,
	0x41, 0xcc, 0xd6, 0x5b, 0xe2, 0x6f, 0x75, 0xf8,
	0x8a, 0x07, 0x1d, 0x90, 0x29, 0xa4, 0xbe, 0x33,
	0xd8, 0x55, 0x4f, 0xc2, 0x7b, 0xf6, 0xec, 0x61,
	0x13, 0x9e, 0x84, 0x09, 0xb0, 0x3d, 0x27, 0xaa,
	0xc3, 0x4e, 0x54, 0xd9, 0x60, 0xed, 0xf7, 0x7a,
	0x08, 0x85, 0x9f, 0x12, 0xab, 0x26, 0x3c, 0xb1,
	0xee, 0x63, 0x79, 0xf4, 0x4d, 0xc0, 0xda, 0x57,
	0x25, 0xa8, 0xb2, 0x3f, 0x86, 0x0b, 0x11, 0x9c,
	0xf5, 0x78, 0x62, 0xef, 0x56, 0xdb, 0xc1, 0x4c,
	0x3e, 0xb3, 0xa9, 0x24, 0x9d, 0x10, 0x0a, 0x87,
	0xb4, 0x39, 0x23, 0xae, 0x17, 0x9a, 0x80, 0x0d,
	0x7f, 0xf2, 0xe8, 0x65, 0xdc, 0x51, 0x4b, 0xc6,
	0xaf, 0x22, 0x38, 0xb5, 0x0c, 0x81, 0x9b, 0x16,
	0x64, 0xe9, 0xf3, 0x7e, 0xc7, 0x4a, 0x50, 0xdd,
	0x82, 0x0f, 0x15, 0x98, 0x21, 0xac, 0xb6, 0x3b,
	0x49, 0xc4, 0xde, 0x53, 0xea, 0x67, 0x7d, 0xf0,
	0x99, 0x14, 0x0e, 0x83, 0x3a, 0xb7, 0xad, 0x20,
	0x52, 0xdf, 0xc5, 0x48, 0xf1, 0x7c, 0x66, 0xeb,
};

static uint8_t generate_crc(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	uint8_t crc = 0xff;

	while (len-- > 0)
		crc = crc8_ccitt[crc ^ *p++];

 	return crc;
}

static inline char d_to_x(int d)
{
	return d < 10 ? d + '0' : (d - 10) + 'a';
}

static inline char x_to_d(char x)
{
	return x >= '0' && x <= '9' ? x - '0' : (x - 'a') + 10;
}

static void prepare_output(struct latmus_net_desc *nd, size_t oseq)
{
	size_t packet_size = nd->packet_size;
	char *packet = nd->tx.packet;
	size_t payloadsz = packet_size - FRAME_METADATA_LEN;
	uint8_t crc;
	int d;

	snprintf(packet, packet_size, "^%.16zx", oseq);
	memset(packet + 17, 'A' + (oseq % 26), payloadsz);
	packet[packet_size - 3] = '$';
	crc = generate_crc(packet, packet_size - 2);
	d = crc % 16;
	packet[packet_size - 1] = d_to_x(d);
	d = crc / 16;
	packet[packet_size - 2] = d_to_x(d);
}

static bool parse_input(struct latmus_net_desc *nd, size_t *next_serial)
{
	size_t packet_size = nd->packet_size;
	char *packet = nd->rx.packet;
	const char *p;
	uint8_t crc;
	int n, d, r;

	/*
	 * packet_size is guaranteed longer than FRAME_METADATA_LEN by
	 * the caller.
	 */
	crc = x_to_d(packet[packet_size - 2]) * 16;
	crc += x_to_d(packet[packet_size - 1]);
	if (generate_crc(packet, packet_size - 2) != crc)
		return false;

	/* Parse the low-hex serial number the manual way. */
	for (n = 16, p = packet + 16, *next_serial = 0, r = 1; n > 0; n--, p--, r *= 16) {
		switch (*p) {
		case '0' ... '9':
			d = *p - '0';
			break;
		case 'a' ... 'f':
			d = (*p - 'a') + 10;
			break;
		default:
			/*
			 * Nah, that would mean the CRC failed to
			 * detect a data corruption.. Anyway, belt
			 * and suspenders.
			 */
			return false;
		}
		*next_serial += (d * r);
	}

	return true;	/* Frame looks good. */
}

static void log_rx_times(struct latmus_net_desc *nd,
			const struct evl_net_iotimes *iotimes,
			unsigned int round)
{
	__s64 dt;

	/* rx_sched: from the driver to the UDP entry point. */
	dt = iotimes->queuing_time - iotimes->device_time;
	add_measurement(&nd->rx.queuing, dt);

	/* rx_user: from the driver to the UDP delivery point. */
	dt = iotimes->delivery_time - iotimes->device_time;
	add_measurement(&nd->rx.delivery, dt);

	if ((round % rounds_per_sec) == 0) {
		__log_results(nd->statistics + RX_QUEUING, &nd->rx.queuing);
		log_results(nd->statistics + RX_DELIVERY, &nd->rx.delivery,
			round / rounds_per_sec);
		reset_measurement(&nd->rx.queuing);
		reset_measurement(&nd->rx.delivery);
		reset_measurement(&nd->tx.queuing);
		reset_measurement(&nd->tx.delivery);
	}
}

static void log_tx_times(struct latmus_net_desc *nd,
			const struct evl_net_iotimes *iotimes,
			int nr,
			unsigned int round)
{
	__s64 dt;
	int n;

	for (n = 0; n < nr; n++, iotimes++) {
		/* tx_dev: from the UDP entry point to the qdisc insertion. */
		dt = iotimes->queuing_time - iotimes->delivery_time;
		add_measurement(&nd->tx.queuing, dt);

		/* tx_usr: from the UDP entry point to the device. */
		dt = iotimes->device_time - iotimes->delivery_time;
		add_measurement(&nd->tx.delivery, dt);
	}

	__log_results(nd->statistics + TX_QUEUING, &nd->tx.queuing);
	__log_results(nd->statistics + TX_DELIVERY, &nd->tx.delivery);
}

static void rx(struct latmus_net_desc *nd, unsigned int seq)
{
	struct evl_net_iotimes iotimes = { 0 };
	struct oob_msghdr msghdr;
	size_t next_serial;
	struct iovec iov;
	ssize_t ret;

	iov.iov_base = nd->rx.packet;
	iov.iov_len = nd->packet_size;
	msghdr.msg_iov = &iov;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = &iotimes;
	msghdr.msg_controllen = sizeof(iotimes);
	msghdr.msg_name = NULL;
	msghdr.msg_namelen = 0;
	msghdr.msg_flags = 0;

	ret = oob_recvmsg(nd->s, &msghdr, NULL, 0);
	if (ret < 0)
		error(1, errno, "oob_recvmsg() failed");

	switch(nd->rx.packet[0]) {
	case '^':
		if (msghdr.msg_controllen == sizeof(iotimes)) /* Did we actually receive a timestamp? */
			log_rx_times(nd, &iotimes, seq);
		if (!parse_input(nd, &next_serial)) {
			nd->rx.invalid_lost++;
			return;
		}
		if (nd->rx.check_sequence &&
			nd->rx.last_serial > 0 &&
			next_serial != nd->rx.last_serial + 1) {
			/*
			 * The CRC guarantees that 'next_serial' was
			 * properly received.
			 */
			if (next_serial > nd->rx.last_serial)
				nd->rx.invalid_lost = next_serial - nd->rx.last_serial - 1;
			else
				nd->rx.invalid_lost++; /* Way too lost in space! */
		}
		nd->rx.last_serial = next_serial;
		break;
	default:
		nd->rx.invalid_lost++;
	}
}

static void tx(struct latmus_net_desc *nd)
{
	int tx_flags = MSG_DONTWAIT;
	struct oob_msghdr msghdr;
	struct iovec iov;
	ssize_t ret;

	iov.iov_base = nd->tx.packet;
	iov.iov_len = nd->packet_size;
	msghdr.msg_iov = &iov;
	msghdr.msg_iovlen = 1;
	msghdr.msg_control = NULL;
	msghdr.msg_controllen = 0;
	msghdr.msg_name = &nd->tx.peer_in;
	msghdr.msg_namelen = sizeof(nd->tx.peer_in);
	msghdr.msg_flags = 0;

	for (;;) {
		ret = oob_sendmsg(nd->s, &msghdr, NULL, tx_flags);
		if (ret < 0) {
			if (errno == EWOULDBLOCK) { /* Overrun? */
				tx_flags = 0; /* Ok, wait next time. */
				nd->tx.delivery.overruns++;
				continue;
			}
			/*
			 * NOTE: receiving EINPROGRESS would be an
			 * error, since we have solicited the peer
			 * already, so we should be able to resolve
			 * its address directly from the oob cache
			 * (that's the point of evl_net_solicit()),
			 * therefore we should never have to downgrade
			 * to in-band transmission.
			 */
			error(1, errno, "oob_sendmsg() failed");
		}
		break;
	}
}

static void timespec_add_ns(struct timespec *__restrict r,
		const struct timespec *__restrict t,
		long ns)
{
	long s, rem;

	s = ns / 1000000000;
	rem = ns - s * 1000000000;
	r->tv_sec = t->tv_sec + s;
	r->tv_nsec = t->tv_nsec + rem;
	if (r->tv_nsec >= 1000000000) {
		r->tv_sec++;
		r->tv_nsec -= 1000000000;
	}
}

static void *net_runner(void *arg)
{
	struct evl_net_iotimes iotimes[16] = { 0 };
	struct latmus_net_desc *nd = arg;
	struct oob_msghdr msghdr;
	struct itimerspec value;
	struct timespec now;
	struct iovec iov;
	int efd, tmfd;
	__u64 ticks;
	ssize_t ret;
	size_t seq;

	efd = evl_attach_self("net-runner:%d", getpid());
	if (efd < 0)
		error(1, -efd, "evl_attach_self() failed");

	tmfd = evl_new_timer(EVL_CLOCK_MONOTONIC);
	if (tmfd < 0)
		error(1, -tmfd, "evl_new_timer() failed");

	ret = evl_set_thread_mode(efd, EVL_T_WOSS, NULL);
	if (ret)
		error(1, -ret, "evl_set_thread_mode(EVL_T_WOSS) failed");

	evl_read_clock(EVL_CLOCK_MONOTONIC, &now);
	timespec_add_ns(&value.it_value, &now, period_usecs * 1000);
	value.it_interval.tv_sec = period_usecs / 1000000;
	value.it_interval.tv_nsec = (period_usecs % 1000000) * 1000;
	ret = evl_set_timer(tmfd, &value, NULL);
	if (ret)
		error(1, -ret, "evl_set_timer() failed");

	for (seq = 0; ; seq++) {
		/* Send a packet. */
		prepare_output(nd, seq);
		tx(nd);

		/* Wait for the next period in the timeline. */
		ret = oob_read(tmfd, &ticks, sizeof(ticks));
		if (ret < 0)
			error(1, -ret, "oob_read(timer) failed");

		/* Try fetching a bulk of pending TX timestamps if any. */
		iov.iov_base = iotimes;
		iov.iov_len = sizeof(iotimes);
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = NULL;
		msghdr.msg_namelen = 0;
		msghdr.msg_flags = 0;
		ret = oob_recvmsg(nd->s, &msghdr, NULL, MSG_TIMESTAMP | MSG_DONTWAIT);
		if (ret < 0) {
			if (errno != EWOULDBLOCK)
				error(1, errno, "oob_recvmsg(MSG_TIMESTAMP) failed");
		} else {
			log_tx_times(nd, iotimes, ret / sizeof(iotimes[0]), seq);
		}

		/* Wait for the next message from the peer. */
		rx(nd, seq);
	}

	return NULL;
}

static int more_net_data(struct statistics *st,
		const struct latmus_measurement *meas)
{
	double tx_dev = 0, tx_usr = 0, tx_best = 0, tx_worst = 0;
	double rx_dev = 0, rx_usr = 0, rx_best = 0, rx_worst = 0;
	struct latmus_net_desc *nd;

	if (verbosity == 0)
		return 0;

	nd = container_of(meas, struct latmus_net_desc, rx.delivery);

	rx_dev = (double)nd->rx.queuing.max_lat / 1000.0;
	rx_usr = (double)nd->rx.delivery.max_lat / 1000.0;
	rx_best = (double)nd->statistics[RX_DELIVERY].all_minlat / 1000.0;
	rx_worst = (double)nd->statistics[RX_DELIVERY].all_maxlat / 1000.0;
	if (nd->tx.queuing.samples > 0)
		tx_dev = (double)nd->tx.queuing.max_lat / 1000.0;
	if (nd->tx.delivery.samples > 0)
		tx_usr = (double)nd->tx.delivery.max_lat / 1000.0;
	if (nd->statistics[TX_DELIVERY].all_samples > 0) {
		tx_best = (double)nd->statistics[TX_DELIVERY].all_minlat / 1000.0;
		tx_worst = (double)nd->statistics[TX_DELIVERY].all_maxlat / 1000.0;
	}

	evl_printf("RTD|%10.3f|%10.3f|%10.3f|%10.3f|%4u|%8u|%10.3f|%10.3f|%10.3f|%10.3f\n",
		rx_dev, rx_usr,
		tx_dev, tx_usr,
		spurious_inband_switches,
		nd->statistics[TX_DELIVERY].all_overruns,
		rx_best, rx_worst,
		tx_best, tx_worst);

	return 0;
}

static void wrap_net_data_page(struct statistics *st, unsigned int round)
{
	time_t now, dt;

	/*
	 * This is called from the RX_DELIVERY logging point
	 * (log_results(nd->statistics + RX_DELIVERY, ...)) after one
	 * second worth of data samples. If the net runner can't cope
	 * with the pace (i.e. TX+RX exceeds the allotted period), the
	 * display may be slowed down to less than 1Hz. If no RX
	 * occurs, the display stalls.
	 */

	time(&now);
	dt = now - start_time;
	evl_printf("RTT|  %.2ld:%.2ld:%.2ld  (%s, %u us period,",
		(long)(dt / 3600), (long)((dt / 60) % 60), (long)(dt % 60),
			context_labels[context_type], period_usecs);
	evl_printf(" priority %d,", responder_priority);
	evl_printf(" CPU%d%s)\n",
		responder_cpu,
		responder_cpu_state & EVL_CPU_ISOL ? "" : "-noisol");
	evl_printf("RTH|%10s|%10s|%10s|%10s|%4s|%8s|%10s|%10s|%10s|%10s\n",
		"--rx sched", "---rx user",
		"--tx sched", "---tx user",
		"-msw", "-overrun",
		"---rx best", "--rx worst",
		"---tx best", "--tx worst");
}

static void print_net_summary(struct statistics *st_array, time_t duration)
{
	time_t t = timeout ?: duration;
	int n;

	for (n = 0; n < NR_STATS; n++)
		if (st_array[n].all_samples == 0)
			return;	/* No significant data. */

	evl_printf("---|----------|----------|----------|----------"
		"|---------------------------------------------------------\n"
		"RTS|%10.3f|%10.3f|%10.3f|%10.3f|%4u|%8u|                          "
		"%.2ld:%.2ld:%.2ld/%.2ld:%.2ld:%.2ld\n",
		(double)st_array[RX_QUEUING].all_maxlat / 1000.0,
		(double)st_array[RX_DELIVERY].all_maxlat / 1000.0,
		(double)st_array[TX_QUEUING].all_maxlat / 1000.0,
		(double)st_array[TX_DELIVERY].all_maxlat / 1000.0,
		spurious_inband_switches,
		st_array[TX_DELIVERY].all_overruns,
		(long)(duration / 3600), (long)((duration / 60) % 60),
		(long)(duration % 60), (long)(duration / 3600),
		(long)((t / 60) % 60), (long)(t % 60));
}

void run_net_test(bool no_check, size_t histogram_cells)
{
	struct sockaddr_in peer_in = { 0 }, local_in = { 0 };
	int ret, s, sig, n, devfd, tsflags;
	struct latmus_net_desc *nd;
	struct sched_param param;
	pthread_attr_t attr;
	pthread_t netrun;
	time_t duration;

	ret = evl_init();
	if (ret)
		error(1, -ret, "evl_init()");

	rounds_per_sec = 1000000 / period_usecs;

	if (!packet_size)
		packet_size = DEFAULT_PACKET_LEN;
	else if (packet_size <= FRAME_METADATA_LEN)
		error(1, EINVAL, "packet size too short (%zd <= %u)",
			packet_size, FRAME_METADATA_LEN);

	peer_in.sin_family = AF_INET;
	peer_in.sin_port = htons(LATMUS_NET_PORT);
	ret = find_host_ip(peer_host, &peer_in.sin_addr);
	if (ret)
		error(1, EINVAL, "cannot resolve '%s' as an IPv4 address",
			peer_host);

	/* evl_net_open_device() only works for oob ports. */
	devfd = evl_net_open_device(local_netif);
	if (devfd < 0)
		error(1, errno, "%s is not an out-of-band networking port",
			local_netif);

	close(devfd);

	local_in.sin_family = AF_INET;
	local_in.sin_port = htons(LATMUS_NET_PORT);
	ret = find_netif_ip(local_netif, &local_in.sin_addr);
	if (ret)
		error(1, -ret, "find_netif_ip(%s)", local_netif);

	/* Get an UDP socket with out-of-band capabilities. */
	s = socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0);
	if (s < 0)
		error(1, errno, "cannot create out-of-band UDP socket");

	/*
	 * Solicit out-of-band networking with the given peer if
	 * requested. Basically, this forces an immediate ARP
	 * resolution of the peer address, and we also ask for making
	 * the resulting entry permanent in the ARP cache.
	 */
	ret = evl_net_solicit(s, (const struct sockaddr *)&peer_in,
			EVL_NEIGH_PERMANENT);
	if (ret) {
		char ip[INET_ADDRSTRLEN];
		error(1, -ret, "%s did not respond",
			inet_ntop(AF_INET, &peer_in.sin_addr, ip, sizeof(ip)));
	}

	/* Enable all existing RX+TX timestamping points. */
	tsflags = EVL_SOF_TIMESTAMPS;
	ret = setsockopt(s, SOL_SOCKET, SO_TIMESTAMP_OOB, &tsflags, sizeof(tsflags));
	if (ret)
		error(1, errno, "setsockopt(SO_TIMESTAMP_OOB)");

	/* Bind the socket to the local interface address. */
	ret = bind(s, (struct sockaddr *)&local_in, sizeof(local_in));
	if (ret < 0)
		error(1, errno, "bind() failed");

	nd = calloc(1, sizeof(*nd));
	if (!nd)
		error(1, ENOMEM, "malloc");

	nd->s = s;
	nd->packet_size = packet_size;
	nd->tx.packet = calloc(1, packet_size);
	nd->rx.packet = calloc(1, packet_size);
	if (!nd->tx.packet || !nd->rx.packet)
		error(1, ENOMEM, "out of memory?");

	nd->tx.peer_in = peer_in;
	nd->rx.check_sequence = !!no_check;

	for (n = 0; n < NR_STATS; n++) {
		nd->statistics[n].ops = (struct __net_stat_ops){
			.more_data = more_net_data,
			.wrap_data_page = wrap_net_data_page,
			.print_summary = print_net_summary,
		};
	}

	reset_measurement(&nd->rx.queuing);
	reset_measurement(&nd->rx.delivery);
	init_statistics("RX queuing", &nd->statistics[RX_QUEUING], histogram_cells);
	init_statistics("RX delivery", &nd->statistics[RX_DELIVERY], histogram_cells);

	reset_measurement(&nd->tx.queuing);
	reset_measurement(&nd->tx.delivery);
	init_statistics("TX queuing", &nd->statistics[TX_QUEUING], histogram_cells);
	init_statistics("TX delivery", &nd->statistics[TX_DELIVERY], histogram_cells);

	/*
	 * Spawn the net runner thread. Our caller pinned us on the
	 * responder CPU, the runner inherits this placement.
	 */
	pthread_attr_init(&attr);
	param.sched_priority = responder_priority;
	pthread_attr_setstacksize(&attr, EVL_STACK_DEFAULT);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	pthread_attr_setschedparam(&attr, &param);
	ret = pthread_create(&netrun, &attr, net_runner, nd);
	pthread_attr_destroy(&attr);
	if (ret)
		error(1, ret, "spawning net runner");

	notify_start(0);
	sigwait(&sigmask, &sig);

	duration = time(NULL) - start_time;
	consume_statistics(nd->statistics, NR_STATS, duration);
}
