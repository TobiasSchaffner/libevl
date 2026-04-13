/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2025 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NET_NET_H
#define _EVL_NET_NET_H

#include <evl/net/device.h>
#include <evl/net/socket.h>

struct sockaddr;

#ifdef __cplusplus
extern "C" {
#endif

int evl_net_open_dev(const char *ifname);

int evl_net_query_dev(int devfd,
		struct evl_net_devstat *devs);

int evl_net_enable_port(int devfd,
			size_t poolsz, size_t bufsz);

int evl_net_disable_port(int devfd);

int evl_net_set_filter(int devfd, const char *modpath);

int evl_net_solicit(int s, const struct sockaddr *peer, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _EVL_NET_NET_H */
