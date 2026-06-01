/*
 * SPDX-License-Identifier: MIT
 *
 * This tidbit demonstrates out-of-band networking, by
 * transmitting[-T]/receiving[-R] UDP packets to/from a particular IP
 * address[-a] and port[-p].
 *
 * The server mode [-S] receives a packet from on a given
 * port and sends a response to the sender's ip/port.
 *
 * The client mode [-C] sends a packet to a port/ip
 * and receives the response from a server. The round
 * trip time is measured.
 *
 * The client and the transmitter wait [-w] before sending the
 * next packet to not flood the network.
 *
 * For the other side, the same code can be used.
 * For transmitting / client on a non-oob port: socat - UDP-LISTEN:<port>
 * For receiving / server on a non-oob port: socat - UDP:<Remote-IP-address>:<port>
 *
 * See https://v4.xenomai.org/core/net/ for details.
 */

#include <pthread.h>
#include <error.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <memory.h>
#include <getopt.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <evl/thread.h>
#include <evl/clock.h>
#include <evl/proxy.h>
#include <evl/net/net.h>

static int verbosity = 1;

static void usage(void)
{
	fprintf(stderr, "oob-net-udp -a <IP-address> [-p <port>]"
		"[-m <text>][-n <msgcount>][-I <iterations>][-i <interface>][-w <wait_time_us>]"
		"[-d][-s][-T|-R|-C|-S][-b]\n");
}

static void print_addr(char* text, struct sockaddr_in *addr){
	char ip_str[INET_ADDRSTRLEN+1];
	inet_ntop(AF_INET, &(addr->sin_addr), ip_str, sizeof(ip_str));
	evl_printf("== %s\n", text);
	evl_printf("   ip-address: %s\n", ip_str);
	evl_printf("   port:       %d\n", ntohs(addr->sin_port));
	evl_printf("   family:     %d\n", addr->sin_family);
}

static void sender(int s, const char *text, int mcount,
		struct sockaddr_in *addr, int iter, useconds_t delay)
{
	struct oob_msghdr msghdr;
	struct iovec iov;
	int n, tlen;
	ssize_t ret;
	char *tbuf;

	tlen = (strlen(text) + 1) * mcount;
	tbuf = malloc(tlen);
	if (!tbuf)
		error(1, ENOMEM, "cannot create message");

	*tbuf = '\0';
	for (n = 0; n < mcount; n++)
		strcat(tbuf, text); /* yep, lazy.. */

	for (n = 0; !iter || n < iter; n++) {
		iov.iov_base = tbuf;
		iov.iov_len = tlen;
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = addr;
		msghdr.msg_namelen = sizeof(*addr);
		msghdr.msg_flags = 0;
		ret = oob_sendmsg(s, &msghdr, NULL, 0);
		if (ret < 0)
			error(1, errno, "oob_sendmsg() failed");

		evl_usleep(delay);
	}

	free(tbuf);
}

static void receiver(int s, struct sockaddr_in *addr, int iter)
{
	struct sockaddr_in _addr;
	socklen_t len = sizeof(_addr);
	struct oob_msghdr msghdr;
	struct iovec iov;
	char rbuf[16384];
	ssize_t ret;
	int n;

	ret = bind(s, (struct sockaddr *)addr, sizeof(*addr));
	if (ret < 0)
		error(1, errno, "bind() failed");

	ret = getsockname(s, (struct sockaddr *)&_addr, &len);
	if (ret < 0)
		error(1, errno, "getsockname() failed");

	if (verbosity)
		print_addr("bind", addr);

	for (n = 0; !iter || n < iter; n++) {
		memset(rbuf, 0, sizeof(rbuf));
		iov.iov_base = rbuf;
		iov.iov_len = sizeof(rbuf);
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = &_addr;
		msghdr.msg_namelen = sizeof(_addr);
		msghdr.msg_flags = 0;
		ret = oob_recvmsg(s, &msghdr, NULL, 0);
		if (ret < 0)
			error(1, errno, "oob_recvmsg() failed");
		evl_printf("= %zd bytes received", ret);
		if (msghdr.msg_flags & MSG_TRUNC)
			evl_printf(" (TRUNCATED)");
		evl_printf(": %.*s\n", (int)ret, rbuf);
	}
}

static void client(int s, const char *text, int mcount,
		struct sockaddr_in *addr, int iter, useconds_t delay)
{
	struct sockaddr_in _addr;
	socklen_t len = sizeof(_addr);
	struct oob_msghdr msghdr;
	struct iovec iov;
	int n, tlen;
	ssize_t ret;
	char *tbuf;
	char rbuf[16384];
	struct timespec ts_tx;
	struct timespec ts_rx;
	double rtt_us = 0.0;

	tlen = (strlen(text) + 1) * mcount;
	tbuf = malloc(tlen);
	if (!tbuf)
		error(1, ENOMEM, "cannot create message");

	*tbuf = '\0';
	for (n = 0; n < mcount; n++)
		strcat(tbuf, text); /* yep, lazy.. */

	ret = connect(s, (struct sockaddr *)addr, sizeof(*addr));
	if (ret < 0)
		error(1, errno, "connect() failed");

	if (verbosity)
		print_addr("send address", addr);

	ret = getsockname(s, (struct sockaddr *)&_addr, &len);
	if (ret < 0)
		error(1, errno, "getsockname() failed");

	if (verbosity)
		print_addr("receive address", &_addr);

	for (n = 0; !iter || n < iter; n++) {
		evl_read_clock(EVL_CLOCK_MONOTONIC, &ts_tx);

		iov.iov_base = tbuf;
		iov.iov_len = tlen;
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = addr;
		msghdr.msg_namelen = sizeof(*addr);
		msghdr.msg_flags = 0;
		ret = oob_sendmsg(s, &msghdr, NULL, 0);
		if (ret < 0)
			error(1, errno, "oob_sendmsg() failed");
		if (verbosity > 1)
			print_addr("sent to", addr);

		memset(rbuf, 0, sizeof(rbuf));
		/* recvmsg stores remote address */
		memset(&_addr, 0, sizeof(_addr));
		iov.iov_base = rbuf;
		iov.iov_len = sizeof(rbuf);
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = &_addr;
		msghdr.msg_namelen = sizeof(_addr);
		msghdr.msg_flags = 0;
		ret = oob_recvmsg(s, &msghdr, NULL, 0);

		evl_read_clock(EVL_CLOCK_MONOTONIC, &ts_rx);
		rtt_us = (ts_rx.tv_sec - ts_tx.tv_sec) * 1000000.0 + (ts_rx.tv_nsec - ts_tx.tv_nsec) / 1000.0;

		if (ret < 0)
			error(1, errno, "oob_recvmsg() failed");
		if (verbosity > 1)
			print_addr("received from", &_addr);
		evl_printf("= %zd bytes received rtt=%.1fus", ret, rtt_us);
		if (msghdr.msg_flags & MSG_TRUNC)
			evl_printf(" (TRUNCATED)");
		evl_printf(": %.*s\n", (int)ret, rbuf);

		evl_usleep(delay);
	}

	free(tbuf);
}

#define SERVER_ADDR_LIST_SIZE 64

static void server(int s, const char *text, int mcount,
		struct sockaddr_in *addr, int iter)
{
	struct sockaddr_in _addr;
	struct oob_msghdr msghdr;
	struct iovec iov;
	int n, tlen;
	ssize_t ret;
	char *tbuf;
	char rbuf[16384];
	in_addr_t addr_list[SERVER_ADDR_LIST_SIZE] = {};
	size_t addr_list_fill = 0;
	bool solicit_done;

	tlen = (strlen(text) + 1) * mcount;
	tbuf = malloc(tlen);
	if (!tbuf)
		error(1, ENOMEM, "cannot create message");

	*tbuf = '\0';
	for (n = 0; n < mcount; n++)
		strcat(tbuf, text); /* yep, lazy.. */

	ret = bind(s, (struct sockaddr *)addr, sizeof(*addr));
	if (ret < 0)
		error(1, errno, "bind() failed");

	if (verbosity)
		print_addr("bind", addr);

	for (n = 0; !iter || n < iter; n++) {
		memset(rbuf, 0, sizeof(rbuf));
		/* recvmsg stores remote address, we respond to this address */
		memset(&_addr, 0, sizeof(_addr));
		iov.iov_base = rbuf;
		iov.iov_len = sizeof(rbuf);
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = &_addr;
		msghdr.msg_namelen = sizeof(_addr);
		msghdr.msg_flags = 0;
		ret = oob_recvmsg(s, &msghdr, NULL, 0);
		if (ret < 0)
			error(1, errno, "oob_recvmsg() failed");
		if (verbosity > 1)
			print_addr("received from", &_addr);
		evl_printf("= %zd bytes received", ret);
		if (msghdr.msg_flags & MSG_TRUNC)
			evl_printf(" (TRUNCATED)");
		evl_printf(": %.*s\n", (int)ret, rbuf);

		/*
		 * We need to call evl_net_solicit for each new client
		 * once before sending data. This demotes the caller to the
		 * in-band stage for the first response.
		 * If this is not done and the ARP address is not
		 * yet in cache or garbage-collected, oob_sendmsg
		 * will return EINPROGRESS on start or during runtime.
		 */
		solicit_done = false;
		for (size_t i = 0; i < addr_list_fill && !solicit_done; i++) {
			if (addr_list[i] == _addr.sin_addr.s_addr) {
				solicit_done = true;
			}
		}
		if (!solicit_done) {
			if (verbosity) {
				char ip_str[INET_ADDRSTRLEN+1];
				inet_ntop(AF_INET, &(_addr.sin_addr), ip_str, sizeof(ip_str));
				evl_printf("== client %s first seen: evl_net_solicit\n", ip_str);
			}
			ret = evl_net_solicit(s, (const struct sockaddr *)&_addr,
					EVL_NEIGH_PERMANENT);
			if (ret)
				error(1, -ret, "evl_net_solicit()");

			if (addr_list_fill < SERVER_ADDR_LIST_SIZE) {
				addr_list[addr_list_fill] = _addr.sin_addr.s_addr;
				addr_list_fill++;
			} else {
				error(1, EPERM, "address list full");
			}
		}

		iov.iov_base = tbuf;
		iov.iov_len = tlen;
		msghdr.msg_iov = &iov;
		msghdr.msg_iovlen = 1;
		msghdr.msg_control = NULL;
		msghdr.msg_controllen = 0;
		msghdr.msg_name = &_addr;
		msghdr.msg_namelen = sizeof(_addr);
		msghdr.msg_flags = 0;
		ret = oob_sendmsg(s, &msghdr, NULL, 0);
		if (ret < 0)
			error(1, errno, "oob_sendmsg() failed");
		if (verbosity > 1)
			print_addr("sent to", &_addr);
	}

	free(tbuf);
}

typedef enum {
	TRANSMIT,
	RECEIVE,
	SERVER,
	CLIENT
} udp_mode_t;

int main(int argc, char *argv[])
{
	int tfd, s, c, mcount = 1, iter = 0, port = 42042, on = 1;
	const char *text = "Mellow sword!\n", *iface = NULL;
	bool bcast = false;
	udp_mode_t mode = RECEIVE;
	struct sched_param param;
	struct sockaddr_in addr;
	const char *ip = NULL;
	ssize_t ret;
	useconds_t delay=1000000;

	while ((c = getopt(argc, argv, "a:m:n:i:w:I:p:dsTRCSb")) != EOF) {
		switch (c) {
		case 'a':
			ip = optarg;
			break;
		case 'd':
			verbosity = 2;
			break;
		case 's':
			verbosity = 0;
			break;
		case 'm':
			text = optarg;
			break;
		case 'n':
			mcount = atoi(optarg);
			break;
		case 'i':
			iface = optarg;
			break;
		case 'w':
			delay = atoi(optarg);
			break;
		case 'I':
			iter = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'b':
			bcast = true; /* Force mode, e.g. for directed broadcast */
			break;
		case 'T':
			mode = TRANSMIT;
			break;
		case 'R':
			mode = RECEIVE;
			break;
		case 'C':
			mode = CLIENT;
			break;
		case 'S':
			mode = SERVER;
			break;
		default:
			usage();
			exit(1);
		}
	}

	if (ip == NULL) {
		usage();
		exit(2);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (!strcmp(ip, "broadcast")) {
		addr.sin_addr.s_addr = INADDR_BROADCAST;
		bcast = true;
	} else if (!inet_pton(AF_INET, ip, &addr.sin_addr)) {
		error(1, EINVAL, "invalid IP address");
	} else if (addr.sin_addr.s_addr == INADDR_BROADCAST) {
		bcast = true;
	}

	param.sched_priority = 1;
	ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
	if (ret)
		error(1, ret, "pthread_setschedparam()");

	tfd = evl_attach_self("oob-net-udp:%d", getpid());
	if (tfd < 0)
		error(1, -tfd, "cannot attach to the EVL core");

	/*
	 * Get an UDP socket with out-of-band capabilities.
	 */
	s = socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0);
	if (s < 0)
		error(1, errno, "cannot create out-of-band UDP socket");

	if (iface) {
		if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, iface, strlen(iface)))
			error(1, errno, "setsockopt(SO_BINDTODEVICE)");
		if (verbosity)
			printf("== bound to %s\n", iface);
	}

	if (mode == TRANSMIT) {
		if (bcast) {
			ret = setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
			if (ret)
				error(1, errno, "cannot enable broadcast for UDP socket");
		}

		/*
		 * Guarantee a mere oob path from the first packet
		 * onward by pre-caching the route and link-layer
		 * address via an explicit neighbour solicitation
		 * before we start sending data.
		 */
		ret = evl_net_solicit(s, (const struct sockaddr *)&addr,
				EVL_NEIGH_PERMANENT);
		if (ret)
			error(1, -ret, "evl_net_solicit()");

		if (verbosity)
			printf("== sender mode (=> %s:%d)\n", bcast ? "[broadcast]" : ip, port);

		sender(s, text, mcount, &addr, iter, delay);
	} else if (mode == RECEIVE) {
		if (verbosity)
			printf("== receiver mode (<= %s:%d)\n", ip, port);

		receiver(s, &addr, iter);
	} else if (mode == CLIENT) {
		if (verbosity)
			printf("== client mode (<=> %s:%d)\n", ip, port);

		/*
		 * Guarantee a mere oob path from the first packet
		 * onward by pre-caching the route and link-layer
		 * address via an explicit neighbour solicitation
		 * before we start sending data.
		 */
		ret = evl_net_solicit(s, (const struct sockaddr *)&addr,
				EVL_NEIGH_PERMANENT);
		if (ret)
			error(1, -ret, "evl_net_solicit()");

		client(s, text, mcount, &addr, iter, delay);
	} else if (mode == SERVER) {
		if (verbosity)
			printf("== server mode (<=> %s:%d)\n", ip, port);

		/*
		 * The server calls evl_net_solicit() internally
		 * for each new ip address.
		 */
		server(s, text, mcount, &addr, iter);
	} else {
		error(1, 0, "Mode not implemented");
	}

	return 0;
}
