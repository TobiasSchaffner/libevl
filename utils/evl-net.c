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
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <evl/compiler.h>
#include <evl/net/net.h>
#include <evl/sys.h>
#include <evl/evl.h>

#define short_optlist "@hedQ::p:b:F::s:S:i:gN::n"

#define SYSFS_VLAN_FILTER	"evl/net/vlans"

static const struct option options[] = {
	{
		.name = "interface",
		.has_arg = required_argument,
		.val = 'i',
	},
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
		.name = "enable-port",
		.has_arg = no_argument,
		.val = 'e',
	},
	{
		.name = "disable-port",
		.has_arg = no_argument,
		.val = 'd',
	},
	{
		.name = "pool-size",
		.has_arg = required_argument,
		.val = 'p',
	},
	{
		.name = "buffer-size",
		.has_arg = required_argument,
		.val = 'b',
	},
	{
		.name = "query-port",
		.has_arg = optional_argument,
		.val = 'Q',
	},
	{
		.name = "allow-gateway",
		.has_arg = no_argument,
		.val = 'g',
	},
	{
		.name = "set-vlan-ids",
		.has_arg = optional_argument,
		.val = 'N',
	},
	{
		.name = "print-vlan-ids",
		.has_arg = required_argument,
		.val = 'n',
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
	fprintf(stderr, "-e -i <ifname> [-p][-b]            enable out-of-band port in network interface <ifname>\n");
	fprintf(stderr, "   -p <pool-size>                  max number of out-of-band socket buffers (0=default)\n");
	fprintf(stderr, "   -b <buffer-size>                size (in bytes) of out-of-band socket buffer (0=default)\n");
	fprintf(stderr, "-d -i <ifname>                     disable out-of-band port in network interface <ifname>\n");
	fprintf(stderr, "-s <host> [-i <ifname>][-g]        solicit <host> via <ifname> if given\n");
	fprintf(stderr, "-S <host> [-i <ifname>][-g]        like -s, marking ARP entry as permanent\n");
	fprintf(stderr, "   -g                              allow routing to destination via gateway(s)\n");
	fprintf(stderr, "-Q[RrTtosfacx] -i <ifname>         query network interface information about <ifname>\n");
	fprintf(stderr, "-F[<bpf-module.o>] -i <ifname>     install/remove eBPF filter (RX)\n");
	fprintf(stderr, "-N[<vlan-id-list]                  add/remove VLAN id. filter (RX)\n");
	fprintf(stderr, "-n                                 show VLAN id. filter (RX)\n");
}

static void enable_oob_port(const char *netif, size_t poolsz, size_t bufsz)
{
	int fd;

	fd = evl_net_enable_port(netif, poolsz, bufsz);
	if (fd < 0)
		error(1, -fd, "cannot enable out-of-band port on %s", netif);

	close(fd);	/* We don't need the fildes of the oob port. */
}

static void disable_oob_port(const char *netif)
{
	int ret, fd;

	fd = evl_net_open_port(netif);
	if (fd < 0)
		error(1, -fd, "cannot open out-of-band port %s", netif);

	ret = evl_net_disable_port(fd);
	if (ret < 0)
		error(1, -ret, "cannot disable out-of-band port on %s", netif);

	close(fd);
}

static void query_oob_port(const char *netif, const char *which)
{
	struct evl_net_devstat devs = { 0 };
	const char *space = "";
	int ret, fd;

	fd = evl_net_open_port(netif);
	if (fd < 0)
		error(1, -fd, "cannot open out-of-band port %s", netif);

	ret = evl_net_query_port(fd, &devs);
	if (ret < 0)
		error(1, -ret, "cannot query out-of-band port on %s", netif);

	close(fd);

	if (!which || !*which) {
		printf("oob capability: %s\n", devs.oob_capable ? "yes" : "no");
		printf("    rx packets: %llu\n", devs.rx_packets);
		printf("      rx bytes: %llu\n", devs.rx_bytes);
		printf("    tx packets: %llu\n", devs.tx_packets);
		printf("      tx bytes: %llu\n", devs.tx_bytes);
		printf("      skb size: %u\n", devs.skb_size);
		printf("      skb free: %u / %u\n", devs.skb_free, devs.skb_total);
		printf("   csum errors: %u\n", devs.csum_errors);
		printf("      rx nomem: %u\n", devs.rx_nomem);
		printf("      tx nomem: %u\n", devs.tx_nomem);
	} else {
		while (*which) {
			switch (*which) {
			case 'R':
				printf("%s%llu", space, devs.rx_packets);
				break;
			case 'r':
				printf("%s%llu", space, devs.rx_bytes);
				break;
			case 'T':
				printf("%s%llu", space, devs.tx_packets);
				break;
			case 't':
				printf("%s%llu", space, devs.tx_bytes);
				break;
			case 'o':
				printf("%s%s", space, devs.oob_capable ? "yes" : "no");
				break;
			case 's':
				printf("%s%u", space, devs.skb_size);
				break;
			case 'f':
				printf("%s%u", space, devs.skb_free);
				break;
			case 'a':
				printf("%s%u", space, devs.skb_total);
				break;
			case 'c':
				printf("%s%u", space, devs.csum_errors);
				break;
			case 'x':
				printf("%s%u %u", space, devs.rx_nomem, devs.tx_nomem);
				break;
			default:
				error(1, EINVAL, "invalid query modifer '%c'", *which);
			}
			space = " ";
			which++;
		}
		putchar('\n');
	}
}

static void set_bpf_filter(const char *netif, const char *modpath)
{
	int ret, fd;

	fd = evl_net_open_port(netif);
	if (fd < 0)
		error(1, -fd, "cannot open out-of-band port %s", netif);

	ret = evl_net_set_filter(fd, modpath);
	if (ret < 0)
		error(1, -ret, "cannot set BPF filter on %s", netif);

	close(fd);

	if (!modpath)
		printf("eBPF module removed from %s\n", netif);
}

static int find_host_ip(const char *host, struct in_addr *addr)
{
	struct addrinfo hints, *res;
	int ret;

	if (!strcmp(host, "broadcast")) {
		addr->s_addr = INADDR_BROADCAST;
		return 0;
	}

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

static void solicit_neighbour(const char *host, const char *netif,
			bool permanent, bool allow_routing)
{
	struct sockaddr addr = { 0 };
	struct sockaddr_in *sin = (struct sockaddr_in *)&addr;
	socklen_t optlen;
	int s, flags;
	long ret;

	ret = find_host_ip(host, &sin->sin_addr);
	if (ret < 0)
		error(1, -ret, "invalid host/IP address");

	s = socket(AF_INET, SOCK_DGRAM | SOCK_OOB, 0);
	if (s < 0)
		error(1, errno, "cannot create out-of-band UDP socket");

	/*
	 * Bind to the given device if any, to force routing via this
	 * interface.
	 */
	optlen = netif ? strlen(netif) + 1 : 0;
	if (optlen && setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, netif, optlen))
		error(1, errno, "cannot bind UDP socket to device %s", netif);

	sin->sin_family = AF_INET;
	/* sin->sin_port is unused. */
	flags = permanent ? EVL_NEIGH_PERMANENT : 0;
	if (allow_routing)
		flags |= EVL_NEIGH_MAYROUTE;
	ret = evl_net_solicit(s, &addr,	flags);
	if (ret)
		error(1, -ret, "%s did not respond", host);

	close(s);
}

static int open_vlan_filter(int mode)
{
	const char *sysfs = getenv("EVL_SYSDIR");
	char *filter_path;
	int ret, fd;

	if (sysfs == NULL)
		error(1, EINVAL, "EVL_SYSDIR unset -- use 'evl net' instead");

	ret = asprintf(&filter_path, "%s/%s", sysfs, SYSFS_VLAN_FILTER);
	if (ret < 0)
		error(1, ENOMEM, "asprintf()");

	fd = open(filter_path, mode);
	if (fd < 0)
		error(1, ENOMEM, "cannot open %s for %s",
			filter_path, mode == O_WRONLY ? "writing" : "reading");

	free(filter_path);

	return fd;
}

static void set_vlan_filter(const char *vlan_list)
{
	ssize_t ret;
	int fd;

	fd = open_vlan_filter(O_WRONLY);

	if (vlan_list)
		ret = write(fd, vlan_list, strlen(vlan_list));
	else
		ret = write(fd, "", 1);

	if (ret < 0)
		error(1, -errno, "cannot set VLAN filters");

	if (!vlan_list)
		printf("VLAN filter cleared\n");

	close(fd);
}

static void show_vlan_filter(void)
{
	ssize_t ret, count = 0;
	char buf[BUFSIZ], *p;
	int fd;

	fd = open_vlan_filter(O_RDONLY);

	do {
		ret = read(fd, buf, sizeof(buf) - 1);
		if (ret < 0)
			error(1, -errno, "cannot read VLAN filters");
		if (ret > 0) {
			count += ret - 1;
			buf[ret] = '\0';
			p = strrchr(buf, '\n');
			if (p)
				*p = '\0';
			printf("%.*s", (int)ret, buf);
		}
	} while (ret == sizeof(buf) - 1);

	if (count > 0)
		putchar('\n');

	close(fd);
}

static void bad_usage(const char *arg0)
{
	usage(arg0);
	exit(1);
}

int main(int argc, char *argv[])
{
	const char *netif = NULL, *modpath = NULL, *ipaddr = NULL,
		*query_type, *vlan_list = NULL;
	bool set_filter = false, solicit = false, permanent = false,
		enable = false, disable = false, query = false,
		allow_routing = false, set_vlans = false,
		show_vlans = false;
	size_t poolsz = 0, bufsz = 0; /* Use defaults. */
	int c, ret;
	char *p;

	if (argc == 1) {
		usage(argv[0]);
		return 0;
	}

	for (;;) {
		c = getopt_long(argc, argv, short_optlist, options, NULL);
		if (c == EOF)
			break;

		switch (c) {
		case 'e':
			enable = true;
			break;
		case 'd':
			disable = true;
			break;
		case 'Q':
			query = true;
			query_type = optarg;
			break;
		case 'p':
			poolsz = strtol(optarg, &p, 10);
			if (*p)
				bad_usage(argv[0]);
			break;
		case 'b':
			bufsz = strtol(optarg, &p, 10);
			if (*p)
				bad_usage(argv[0]);
			break;
		case 'S':
			permanent = true;
			__fallthrough;
		case 's':
			ipaddr = optarg;
			solicit = true;
			break;
		case 'F':
			modpath = optarg;
			set_filter = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		case 'i':
			netif = optarg;
			break;
		case 'g':
			allow_routing = true;
			break;
		case 'N':
			vlan_list = optarg;
			set_vlans = true;
			break;
		case 'n':
			show_vlans = true;
			break;
		case '@':
			printf("manage the EVL out-of-band networking stack\n");
			return 0;
		default:
			bad_usage(argv[0]);
		}
	}

	if (optind < argc)
		bad_usage(argv[0]);

	ret = evl_init();	/* Make sure we have the ABI right. */
	if (ret)
		error(1, -ret, "evl_init()");

	if (!(disable || enable || query || solicit || set_filter ||
			set_vlans || show_vlans)) {
		fprintf(stderr, "no command given\n");
		bad_usage(argv[0]);
	}

	if ((disable || enable || query || set_filter) && !netif) {
		fprintf(stderr, "a network interface must be specified\n");
		bad_usage(argv[0]);
	}

	if (disable)
		disable_oob_port(netif);

	if (enable)
		enable_oob_port(netif, poolsz, bufsz);

	if (set_filter)
		set_bpf_filter(netif, modpath);

	if (set_vlans)
		set_vlan_filter(vlan_list);

	if (show_vlans)
		show_vlan_filter();

	if (solicit)
		solicit_neighbour(ipaddr, netif, permanent, allow_routing);

	if (query)
		query_oob_port(netif, query_type);

	return 0;
}
