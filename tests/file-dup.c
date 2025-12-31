/*
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <evl/thread.h>
#include <evl/xbuf.h>
#include "helpers.h"

int main(int argc, char *argv[])
{
	int tfd, xfdo, xfdi, pipefd[2], fd, maxfd;
	char *name, *xpath, buf[3] = { 0 };
	ssize_t ret;

	__Tcall_assert(tfd, evl_attach_self("file-dup:%d", getpid()));
	signal(SIGPIPE, SIG_IGN);

	name = get_unique_name_and_path(EVL_XBUF_DEV, 0, &xpath);

	/* dup() src=oob_fd */
	__Tcall_assert(xfdo, evl_new_xbuf(1024, "%s", name));
	__Tcall_assert(xfdi, open(xpath, O_RDWR));
	__Tcall_assert(fd, dup(xfdo));
	__Tcall_errno_assert(ret, oob_write(fd, "FoO", 3));
	__Tcall_errno_assert(ret, read(xfdi, buf, 3));
	__Texpr_assert(!memcmp(buf, "FoO", 3));
	__Tcall_errno_assert(ret, close(xfdo));
	__Tcall_errno_assert(ret, close(xfdi));
	__Tcall_errno_assert(ret, close(fd));

	/* dup2() src=oob_fd, dst=regular_fd */
	__Tcall_assert(xfdo, evl_new_xbuf(1024, "%s", name));
	__Tcall_assert(xfdi, open(xpath, O_RDWR));
	__Tcall_assert(ret, pipe(pipefd));
	__Tcall_assert(fd, dup2(xfdo, pipefd[0]));
	__Tcall_errno_assert(ret, oob_write(fd, "BaR", 3));
	__Tcall_errno_assert(ret, read(xfdi, buf, 3));
	__Texpr_assert(!memcmp(buf, "BaR", 3));
	__Tcall_errno_assert(ret, close(pipefd[0]));
	__Tcall_errno_assert(ret, close(pipefd[1]));
	__Tcall_errno_assert(ret, close(xfdo));
	__Tcall_errno_assert(ret, close(xfdi));

	/* dup2() src=regular_fd, dst=oob_fd */
	__Tcall_assert(xfdo, evl_new_xbuf(1024, "%s", name));
	__Tcall_assert(xfdi, open(xpath, O_RDWR));
	__Tcall_assert(ret, pipe(pipefd));
	__Tcall_assert(fd, dup2(pipefd[1], xfdo));
	__Tcall_errno_assert(ret, write(fd, "QuX", 3));
	__Tcall_errno_assert(ret, read(pipefd[0], buf, 3));
	__Texpr_assert(!memcmp(buf, "QuX", 3));
	__Tcall_errno_assert(ret, close(pipefd[0]));
	__Tcall_errno_assert(ret, close(pipefd[1]));
	__Tcall_errno_assert(ret, close(xfdo));
	__Tcall_errno_assert(ret, close(xfdi));

	/* dup2() src=oob_fd, dst=oob_fd */
	__Tcall_assert(xfdo, evl_new_xbuf(1024, "%s", name));
	__Tcall_assert(xfdi, open(xpath, O_RDWR));
	__Tcall_assert(fd, dup(xfdo));
	__Tcall_assert(fd, dup2(xfdi, fd));
	__Tcall_errno_assert(ret, oob_write(xfdo, "BaZ", 3));
	__Tcall_errno_assert(ret, read(xfdi, buf, 3));
	__Texpr_assert(!memcmp(buf, "BaZ", 3));
	__Tcall_errno_assert(ret, close(xfdo));
	__Tcall_errno_assert(ret, close(xfdi));
	__Tcall_errno_assert(ret, close(fd));

	/*
	 * Force an expansion of the (oob) fdtable by allocating as
	 * many file descriptors as we can.
	 */
	maxfd = fd = dup(0);
	for (;;) {
		ret = dup(fd);
		if (ret < 0) {
			__Texpr_assert(errno == EMFILE);
			break;
		}
		maxfd = ret;
	}

	do {
		__Tcall_errno_assert(ret, close(fd));
	} while (++fd <= maxfd);

	return 0;
}
