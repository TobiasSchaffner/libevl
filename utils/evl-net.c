/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2024 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdbool.h>
#include <stdio.h>
#include <error.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <evl/compiler.h>
#include <evl/net/net.h>
#include <evl/sys.h>

#define short_optlist "@hF::s:S:i:"

static const struct option options[] = {
	{
		.name = "filter",
		.has_arg = optional_argument,
		.val = 'F',
	},
	{
		.name = "solicit",
		.has_arg = required_argument,
		.val = 's',
	},
	{
		.name = "solicit-permanent",
		.has_arg = required_argument,
		.val = 'S',
	},
	{
		.name = "interface",
		.has_arg = required_argument,
		.val = 'i',
	},
	{
		.name = "help",
		.has_arg = no_argument,
		.val = 'h',
	},
	{ /* Sentinel */ }
};

static void usage(const char *arg0)
{
        fprintf(stderr, "usage: %s [options]:\n", basename(arg0));
	fprintf(stderr, "-F[<bpf-module.o>] -i <network-interface>     install/remove eBPF filter (RX)\n");
	fprintf(stderr, "-s <ipaddr>                                   neighbour solicitation with <ipaddr>\n");
	fprintf(stderr, "-S <ipaddr>                                   neighbour solicitation with <ipaddr> (set permanent)\n");
}

static void set_bpf_filter(const char *netif, const char *modpath)
{
	int ret;

	ret = evl_net_set_filter(netif, modpath);
	if (ret < 0)
		error(1, -ret, "cannot set BPF filter on '%s'", netif);
}

static void solicit_neighbour(const char *ipaddr, bool permanent)
{
	struct sockaddr addr = { 0 };
	struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
	long ret;
	int s;

	if (!inet_pton(AF_INET, ipaddr, &sin->sin_addr))
		error(1, EINVAL, "invalid IP address");

	s = socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0);
	if (s < 0)
		error(1, errno, "cannot create out-of-band UDP socket");

	sin->sin_family = AF_INET;
	/* sin->sin_port is unused. */
	ret = evl_net_solicit(s, &addr,	permanent ? EVL_NEIGH_PERMANENT : 0);
	if (ret)
		error(1, -ret, "evl_net_solicit()");

	close(s);
}

static void bad_usage(const char *arg0)
{
	usage(arg0);
	exit(1);
}

int main(int argc, char *argv[])
{
	bool set_filter = false, solicit = false, permanent = false;
	const char *netif = NULL, *modpath = NULL, *ipaddr = NULL;
	int c;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	for (;;) {
		c = getopt_long(argc, argv, short_optlist, options, NULL);
		if (c == EOF)
			break;

		switch (c) {
		case 'i':
			netif = optarg;
			break;
		case 'F':
			modpath = optarg;
			set_filter = true;
			break;
		case 'S':
			permanent = true;
			__fallthrough;
		case 's':
			ipaddr = optarg;
			solicit = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case '@':
			printf("manage the EVL out-of-band networking stack\n");
			return 0;
		default:
			bad_usage(argv[0]);
		}
	}

	if (optind < argc)
		bad_usage(argv[0]);

	if (set_filter) {
		if (!netif || (ipaddr && !solicit))
			bad_usage(argv[0]);
		set_bpf_filter(netif, modpath);
	}

	if (solicit) {
		if (netif && !set_filter)
			bad_usage(argv[0]);
		solicit_neighbour(ipaddr, permanent);
	}

	return 0;
}
