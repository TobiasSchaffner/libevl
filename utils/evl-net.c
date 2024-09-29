/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2024 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdio.h>
#include <error.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <evl/net/device.h>
#include <evl/sys.h>

#define short_optlist "@hF::i:"

static const struct option options[] = {
	{
		.name = "filter",
		.has_arg = optional_argument,
		.val = 'F',
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
}

static void set_bpf_filter(const char *netif, const char *modpath)
{
	int netfd, ret, devfd, progfd;
	struct evl_net_devfd req;
	struct bpf_program *prog;
	struct bpf_object *obj;
	long err;

	netfd = evl_open_raw(EVL_NET_DEV);
	if (netfd < 0)
		error(1, errno, "cannot open EVL '%s' device", EVL_NET_DEV);

	/*
	 * Get a file descriptor to the network device for EVL-related
	 * operations. Lookup is performed by name.
	 */
	req.name_ptr = (__u64)(uintptr_t)netif;
	ret = ioctl(netfd, EVL_NET_GETDEVFD, &req);
	if (ret < 0)
		error(1, errno, "ioctl(EVL_NET_GETDEVFD)");

	devfd = req.fd;

	if (!modpath) {
		progfd = -1;
		ret = ioctl(devfd, EVL_NDEVIOC_SETRXEBPF, &progfd);
		if (ret)
			error(1, errno, "ioctl(EVL_NDEVIOC_SETRXEBPF)");
	} else {
		obj = bpf_object__open_file(modpath, NULL);
		err = libbpf_get_error(obj);
		if (err)
			error(1, err, "cannot open %s for reading", modpath);

		err = bpf_object__load(obj);
		if (err)
			error(1, err, "cannot load %s", modpath);

		/*
		 * If multiple programs are available from the module,
		 * only the last one gets installed.
		 */
		bpf_object__for_each_program(prog, obj) {
			progfd = bpf_program__fd(prog);
			ret = ioctl(devfd, EVL_NDEVIOC_SETRXEBPF, &progfd);
			if (ret)
				error(1, errno, "ioctl(EVL_NDEVIOC_SETRXEBPF)");
		}
	}
}

static void bad_usage(const char *arg0)
{
	usage(arg0);
	exit(1);
}

int main(int argc, char *argv[])
{
	const char *netif = NULL, *modpath = NULL;
	bool set_filter = false;
	int c;

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
		case 'h':
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
		if (!netif)
			bad_usage(argv[0]);
		set_bpf_filter(netif, modpath);
	}

	return 0;
}
