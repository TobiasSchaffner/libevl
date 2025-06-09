/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdint.h>
#include <memory.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <evl/net/net.h>
#include <evl/sys.h>

int evl_net_open_device(const char *ifname)
{
	struct evl_net_devfd req;
	int ret, fd;

	fd = evl_open_raw(EVL_NET_DEV);
	if (fd < 0)
		return -errno;

	/*
	 * Get a file descriptor to the network device for EVL-related
	 * operations. Lookup is performed by name.
	 */
	memset(&req, 0, sizeof(req));
	req.name_ptr = (__u64)(uintptr_t)ifname;
	ret = ioctl(fd, EVL_NET_GETDEVFD, &req);
	close(fd);
	if (ret < 0)
		return -errno;

	return req.fd;
}

int evl_net_solicit(int s, const struct sockaddr *peer, int flags)
{
	struct evl_net_solicit solicit;
	int ret;

	memset(&solicit, 0, sizeof(solicit));
	solicit.addr = *peer;
	solicit.flags = flags;
	ret = ioctl(s, EVL_SOCKIOC_SOLICIT, &solicit);

	return ret ? -errno : 0;
}
