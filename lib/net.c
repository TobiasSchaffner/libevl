/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdint.h>
#include <memory.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <evl/net/net.h>
#include <evl/sys.h>

int evl_net_open_dev(const char *ifname)
{
	struct evl_net_devopen req;
	int ret, netfd;

	netfd = evl_open_raw(EVL_NET_DEV);
	if (netfd < 0)
		return errno == ENOENT ? -ENOTSUP : -errno;

	/*
	 * Get a file descriptor to the network device for EVL-related
	 * operations. Lookup is performed by name.
	 */
	memset(&req, 0, sizeof(req));
	req.name_ptr = (__u64)(uintptr_t)ifname;
	ret = ioctl(netfd, EVL_NETIOC_DEVOPEN, &req);
	close(netfd);
	if (ret < 0)
		return -errno;

	return req.fd;
}

int evl_net_enable_port(int devfd, size_t poolsz, size_t bufsz)
{
	struct evl_net_devparams req = {
		.poolsz = poolsz,
		.bufsz = bufsz,
	};
	int ret;

	ret = ioctl(devfd, EVL_NDEVIOC_SETPORT, &req);
	if (ret)
		ret = -errno;

	return ret;
}

int evl_net_disable_port(int devfd)
{
	return ioctl(devfd, EVL_NDEVIOC_SETPORT, NULL) ?
		(errno == ENOTTY ? -EBADF : -errno) : 0;
}

int evl_net_set_filter(int devfd, const char *modpath)
{
	struct bpf_program *prog;
	struct bpf_object *obj;
	int progfd;
	long ret;

	if (!modpath) {		/* Uninstall. */
		progfd = -1;
		return ioctl(devfd, EVL_NDEVIOC_SETRXEBPF, &progfd) ?
			(errno == ENOTTY ? -EBADF : -errno) : 0;
	}

	obj = bpf_object__open_file(modpath, NULL);
	if (!obj)
		return -errno;

	ret = bpf_object__load(obj);
	if (ret)
		goto out;

	/*
	 * If multiple programs are available from the module, the
	 * first one is installed.
	 */
	prog = bpf_object__next_program(obj, NULL);
	if (prog) {
		progfd = bpf_program__fd(prog);
		if (ioctl(devfd, EVL_NDEVIOC_SETRXEBPF, &progfd))
			ret = errno == ENOTTY ? -EBADF : -errno;
	}
out:
	bpf_object__close(obj);

	return ret;
}

int evl_net_query_dev(int devfd, struct evl_net_devstat *devs)
{
	return ioctl(devfd, EVL_NDEVIOC_GETSTAT, devs) ?
		(errno == ENOTTY ? -EBADF : -errno) : 0;
}

int evl_net_solicit(int s, const struct sockaddr *peer, int flags)
{
	struct evl_net_solicit solicit;
	int ret;

	memset(&solicit, 0, sizeof(solicit));
	memcpy(&solicit.addr, peer, sizeof(*peer));
	solicit.flags = flags;
	ret = ioctl(s, EVL_SOCKIOC_SOLICIT, &solicit);

	return ret ? (errno == ENOTTY ? -EBADF : -errno) : 0;
}
