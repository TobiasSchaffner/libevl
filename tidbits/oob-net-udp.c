/*
 * SPDX-License-Identifier: MIT
 *
 * This tidbit demonstrates out-of-band networking, by
 * sending[-S]/receiving[-R] UDP packets to/from a particular IP
 * address[-a] and port[-p].
 *
 * See https://v4.xenomai.org/core/net/ for details.
 */

#include <pthread.h>
#include <error.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
#include <evl/net/socket.h>

static int verbosity = 1;

static void usage(void)
{
	fprintf(stderr, "oob-net-udp -a <IP-address> [-p <port>][-m <text>][-n <msgcount>][-i <iterations>][-d][-s][-R|-S]\n");
}

static void sender(int s, const char *text, int mcount,
		struct sockaddr_in *addr, int iter)
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

		evl_usleep(1000000);
	}
}

static void receiver(int s, struct sockaddr_in *addr, int iter)
{
	struct sockaddr_in _addr;
	socklen_t len = sizeof(_addr);
	struct oob_msghdr msghdr;
	struct iovec iov;
	char tbuf[16384];
	ssize_t ret;
	int n;

	ret = bind(s, (struct sockaddr *)addr, sizeof(*addr));
	if (ret < 0)
		error(1, errno, "bind() failed");

	ret = getsockname(s, (struct sockaddr *)&_addr, &len);
	if (ret < 0)
		error(1, errno, "getsockname() failed");

	if (verbosity)
		printf("== bound to port %d\n", ntohs(_addr.sin_port));

	for (n = 0; !iter || n < iter; n++) {
		memset(tbuf, 0, sizeof(tbuf));
		iov.iov_base = tbuf;
		iov.iov_len = sizeof(tbuf);
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
		evl_printf(": %.*s\n", ret, tbuf);
	}
}

int main(int argc, char *argv[])
{
	int tfd, s, c, mcount = 1, iter = 0, port = 42042;
	const char *text = "Mellow sword!";
	struct evl_net_solicit solicit;
	struct sched_param param;
	struct sockaddr_in addr;
	const char *ip = NULL;
	bool send = false;
	ssize_t ret;

	while ((c = getopt(argc, argv, "a:m:n:i:p:dsRS")) != EOF) {
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
			iter = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'R':
			send = false;
			break;
		case 'S':
			send = true;
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
	if (!inet_pton(AF_INET, ip, &addr.sin_addr))
		error(1, EINVAL, "invalid IP address");

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

	if (send) {
		/*
		 * Guarantee a mere oob path from the first packet
		 * onward by pre-caching the route and link-layer
		 * address via an explicit neighbour solicitation
		 * before we start sending data.
		 */
		memset(&solicit, 0, sizeof(solicit));
		solicit.addr.sa_family = AF_INET;
		*((struct sockaddr_in *)&solicit.addr) = addr;
		solicit.flags = EVL_NEIGH_PERMANENT;
		ret = ioctl(s, EVL_SOCKIOC_SOLICIT, &solicit);
		if (ret)
			error(1, errno, "ioctl(EVL_SOCKIOC_SOLICIT)");

		if (verbosity)
			printf("== sender mode (=> %s:%d)\n", ip, port);

		sender(s, text, mcount, &addr, iter);
	} else {
		if (verbosity)
			printf("== receiver mode (<= %s:%d)\n", ip, port);

		receiver(s, &addr, iter);
	}

	return 0;
}
