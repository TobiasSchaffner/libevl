/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_SOCKET_H
#define _EVL_NET_SOCKET_H

#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <evl/fcntl.h>
#include <evl/net/socket-abi.h>

struct oob_msghdr {
	void		*msg_name;
	socklen_t	msg_namelen;
	struct iovec	*msg_iov;
	size_t		msg_iovlen;
	void		*msg_control;
	size_t		msg_controllen;
	int		msg_flags;
};

#ifdef __cplusplus
extern "C" {
#endif

ssize_t oob_recvmsg(int sockfd, struct oob_msghdr *msghdr,
		    const struct timespec *timeout,
		    int flags);

ssize_t oob_sendmsg(int sockfd, const struct oob_msghdr *msghdr,
		    const struct timespec *timeout,
		    int flags);

int oob_setsockopt(int sockfd, int level, int optname,
		const void *optval,
		socklen_t optlen);

int oob_getsockopt(int sockfd, int level, int optname,
		void *optval,
		socklen_t *optlen);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_NET_SOCKET_H */
