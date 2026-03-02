/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 *
 * An EVL socket is basically a regular socket which the EVL core
 * extends to support out-of-band communications.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <memory.h>
#include <evl/syscall.h>
#include <evl/net/socket.h>
#include "internal.h"

ssize_t oob_recvmsg(int sockfd, struct oob_msghdr *msghdr,
		const struct timespec *timeout,
		int flags)
{
	struct user_oob_msghdr u_msghdr;
	long ret;

	u_msghdr.iov_ptr = __evl_ptr64(msghdr->msg_iov);
	u_msghdr.iovlen = (__u32)msghdr->msg_iovlen;
	u_msghdr.ctl_ptr = __evl_ptr64(msghdr->msg_control);
	u_msghdr.ctllen = (__u32)msghdr->msg_controllen;
	u_msghdr.name_ptr = __evl_ptr64(msghdr->msg_name);
	u_msghdr.namelen = (__u32)msghdr->msg_namelen;
	u_msghdr.count = 0;
	u_msghdr.flags = flags;	/* in/out */
	u_msghdr.timeout_ptr = __evl_ktimespec_ptr64(timeout);

	ret = oob_ioctl(sockfd, EVL_SOCKIOC_RECVMSG, &u_msghdr);
	if (ret)
		return -1;	/* Status in errno. */

	msghdr->msg_namelen = u_msghdr.namelen;
	msghdr->msg_controllen = u_msghdr.ctllen;
	msghdr->msg_flags = u_msghdr.flags;

	return (__ssize_t)u_msghdr.count;
}

ssize_t oob_sendmsg(int sockfd, const struct oob_msghdr *msghdr,
		const struct timespec *timeout,
		int flags)
{
	struct user_oob_msghdr u_msghdr;
	long ret;

	u_msghdr.iov_ptr = __evl_ptr64(msghdr->msg_iov);
	u_msghdr.iovlen = (__u32)msghdr->msg_iovlen;
	u_msghdr.ctl_ptr = __evl_ptr64(msghdr->msg_control);
	u_msghdr.ctllen = (__u32)msghdr->msg_controllen;
	u_msghdr.name_ptr = __evl_ptr64(msghdr->msg_name);
	u_msghdr.namelen = (__u32)msghdr->msg_namelen;
	u_msghdr.count = 0;
	u_msghdr.flags = flags;	/* in */
	u_msghdr.timeout_ptr = __evl_ktimespec_ptr64(timeout);

	ret = oob_ioctl(sockfd, EVL_SOCKIOC_SENDMSG, &u_msghdr);
	if (ret)
		return -1;	/* Status in errno. */

	return (__ssize_t)u_msghdr.count;
}

int oob_setsockopt(int sockfd, int level, int optname,
		const void *optval,
		socklen_t optlen)
{
	struct evl_net_sockopt opt;

	opt.level = level;
	opt.option = optname;
	opt.optval_ptr = __evl_ptr64(optval);
	opt.optlen_ptr = __evl_ptr64(&optlen);

	return oob_ioctl(sockfd, EVL_SOCKIOC_SETOPT, &opt);
}

int oob_getsockopt(int sockfd, int level, int optname,
		void *optval,
		socklen_t *optlen)
{
	struct evl_net_sockopt opt;

	opt.level = level;
	opt.option = optname;
	opt.optval_ptr = __evl_ptr64(optval);
	opt.optlen_ptr = __evl_ptr64(optlen);

	return oob_ioctl(sockfd, EVL_SOCKIOC_GETOPT, &opt);
}
