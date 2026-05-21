/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <memory.h>
#include <valgrind/valgrind.h>
#include <evl/sys.h>
#include <evl/version.h>
#include <evl/net/device.h>
#include <evl/factory-abi.h>
#include <evl/signal-abi.h>
#include <evl/control-abi.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <asm/evl/vdso.h>
#include "parse_vdso.h"
#include "internal.h"

#ifndef EVL_ABI_BASE
/*
 * EVL_ABI_BASE was not defined prior to ABI 18, until support for ABI
 * ranges was introduced in the core.
 */
#define EVL_ABI_BASE  17
#else
#endif
#if !(EVL_ABI_PREREQ >= EVL_ABI_BASE && EVL_ABI_PREREQ <= EVL_ABI_LEVEL)
#error "kernel does not meet our ABI requirements (uapi vs EVL_ABI_PREREQ)"
#endif

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static int init_status;

static struct evl_core_info core_info;

int __evl_ctlfd = -1;

void *__evl_shared_memory = NULL;

static void atfork_unmap_shmem(void)
{
	if (__evl_shared_memory) {
		munmap(__evl_shared_memory, core_info.shm_size);
		__evl_shared_memory = NULL;
	}

	if (__evl_ctlfd >= 0) {
		close(__evl_ctlfd);
		__evl_ctlfd = -1;
	}

	init_once = PTHREAD_ONCE_INIT;
}

static inline int generic_init(void)
{
	int ctlfd, ret;
	void *shmem;

	/*
	 * Failing to open the control device with ENOENT is a clear
	 * sign that we have no EVL core in there. Return with -ENOSYS
	 * to give a clear hint about this.
	 */
	ctlfd = open(EVL_CONTROL_DEV, O_RDWR);
	if (ctlfd < 0) {
		if (errno == ENOENT) {
			fprintf(stderr,	"evl: core not enabled in kernel\n");
			return -ENOSYS;
		}
		return -errno;
	}

	ret = fcntl(ctlfd, F_GETFD, 0);
	if (ret < 0) {
		ret = -errno;
		goto fail;
	}

	ret = fcntl(ctlfd, F_SETFD, ret | O_CLOEXEC);
	if (ret < 0) {
		ret = -errno;
		goto fail;
	}

	ret = ioctl(ctlfd, EVL_CTLIOC_GET_COREINFO, &core_info);
	if (ret) {
		/*
		 * sizeof(core_info) is encoded into
		 * EVL_CTLIOC_GET_COREINFO, in which case we might
		 * receive ENOTTY if some ABI change involved updating
		 * the core info struct itself.
		 */
		if (errno != ENOTTY) {
			ret = -errno;
			goto fail;
		}
		/*
		 * core_info was not filled in, make sure we catch the
		 * ABI discrepancy.
		 */
		core_info.abi_base = (__u32)-1;
	}

	if (EVL_ABI_PREREQ < core_info.abi_base ||
		EVL_ABI_PREREQ > core_info.abi_current) {
		fprintf(stderr,
			"evl: ABI mismatch, see -ENOEXEC at https://v4.xenomai.org/"
			"core/user-api/init/#evl_init\n");
		ret = -ENOEXEC;
		goto fail;
	}

	ret = __evl_attach_clocks();
	if (ret)
		goto fail;

	shmem = mmap(NULL, core_info.shm_size, PROT_READ|PROT_WRITE,
		MAP_SHARED, ctlfd, 0);
	if (shmem == MAP_FAILED) {
		ret = -errno;
		goto fail;
	}

	pthread_atfork(NULL, NULL, atfork_unmap_shmem);
	__evl_ctlfd = ctlfd;
	__evl_shared_memory = shmem;

	return 0;
fail:
	close(ctlfd);

	return ret;
}

static void resolve_vdso_calls(void)
{
	/*
	 * We have no vDSO if running on Valgrind, always use fallback
	 * calls.
	 */
	if (RUNNING_ON_VALGRIND)
		return;

	evl_init_vdso();

	__evl_clock_gettime = evl_request_vdso(__EVL_VDSO_KVERSION,
					__EVL_VDSO_GETTIME);
}

static int do_init(void)
{
	int ret;

	resolve_vdso_calls();

	ret = mlockall(MCL_CURRENT | MCL_FUTURE);
	if (ret)
		return -errno;

	ret = generic_init();
	if (ret)
		return ret;

	__evl_setup_proxies();

	return 0;
}

static void do_init_once(void)
{
	init_status = do_init();
}

static void do_lart_once(void)
{
	fprintf(stderr,	"evl: core present but stopped\n");
}

static void lart_once(void)
{
	static pthread_once_t lart_once = PTHREAD_ONCE_INIT;
	pthread_once(&lart_once, do_lart_once);
}

static int flip_fd_flags(int efd, int cmd, int flags)
{
	int ret;

	ret = fcntl(efd, cmd == F_SETFD ? F_GETFD : F_GETFL, 0);
	if (ret < 0)
		return -errno;

	ret = fcntl(efd, cmd, ret | flags);
	if (ret)
		return -errno;

	return 0;
}

#define raw_write_out(__msg)					\
	do {							\
		int __ret;					\
		__ret = write(1, __msg , strlen(__msg));	\
		(void)__ret;					\
	} while (0)

static const char *sigdebug_msg[] = {
	[EVL_HMDIAG_SIGDEMOTE] = "switched inband (signal)\n",
	[EVL_HMDIAG_SYSDEMOTE] = "switched inband (syscall)\n",
	[EVL_HMDIAG_EXDEMOTE] = "switched inband (fault)\n",
	[EVL_HMDIAG_LKDEPEND] = "switched inband while holding mutex\n",
	[EVL_HMDIAG_WATCHDOG] = "watchdog triggered\n",
	[EVL_HMDIAG_LKIMBALANCE] = "mutex lock/unlock imbalance\n",
	[EVL_HMDIAG_LKSLEEP] = "attempt to sleep while holding a mutex\n",
	[EVL_HMDIAG_STAGEX] = "locked out from out-of-band stage (stax)\n",
};

/* A basic SIGDEBUG handler which only prints out the cause. */

void evl_sigdebug_handler(int sig, siginfo_t *si, void *ctxt)
{
	if (sigdebug_marked(si)) {
		switch (sigdebug_cause(si)) {
		case EVL_HMDIAG_SIGDEMOTE:
		case EVL_HMDIAG_SYSDEMOTE:
		case EVL_HMDIAG_EXDEMOTE:
		case EVL_HMDIAG_LKDEPEND:
		case EVL_HMDIAG_WATCHDOG:
		case EVL_HMDIAG_LKIMBALANCE:
		case EVL_HMDIAG_LKSLEEP:
		case EVL_HMDIAG_STAGEX:
			raw_write_out(sigdebug_msg[sigdebug_cause(si)]);
			break;
		}
	}
}

int evl_init(void)
{
	pthread_once(&init_once, do_init_once);

	return init_status;
}

unsigned int evl_detect_fpu(void)
{
	if (evl_init())
		return 0;

	return core_info.fpu_features;
}

/*
 * Creating an EVL element is done in the following steps:
 *
 * 1. open the clone device of the proper element class.
 *
 * 2. issue ioctl(EVL_IOC_CLONE) to create a new element, passing
 * an attribute structure.
 *
 * 3. if EVL_CLONE_PUBLIC was mentioned in the clone_flags, open the
 * new element device to get a file descriptor on it; otherwise an
 * open descriptor is returned by EVL_IOC_CLONE for private
 * elements.
 *
 * Except for threads, closing the last file descriptor referring to
 * an element causes its automatic deletion.
 */
int evl_create_element(const char *type, const char *name,
		void *attrs, int clone_flags,
		struct evl_element_ids *eids)
{
	char *fdevname, *edevname = NULL;
	struct evl_clone_req clone;
	int ffd, efd, ret;
	bool nonblock;

	nonblock = !!(clone_flags & EVL_CLONE_NONBLOCK);
	/* Strip off user-only bits. */
	clone_flags &= EVL_CLONE_MASK;
	clone_flags &= ~EVL_CLONE_NONBLOCK;

	ret = asprintf(&fdevname, "/dev/evl/%s/clone", type);
	if (ret < 0)
		return -ENOMEM;

	ffd = open(fdevname, O_RDWR);
	if (ffd < 0) {
		ret = -errno;
		goto out_factory;
	}

	/*
	 * Turn on public mode if the user-provided name starts with a
	 * slash.  Anonymous elements must be private by definition.
	 */
	if (name == NULL) {
		if (clone_flags & EVL_CLONE_PUBLIC)
			return -EINVAL;
	} else if (*name == '/') {
		clone_flags |= EVL_CLONE_PUBLIC;
		name++;
	}

	memset(&clone, 0, sizeof(clone)); /* To please valgrind.. */
	clone.name_ptr = __evl_ptr64(name);
	clone.attrs_ptr = __evl_ptr64(attrs);
	clone.clone_flags = clone_flags;
	ret = ioctl(ffd, EVL_IOC_CLONE, &clone);
	if (ret) {
		ret = -errno;
		if (ret == -ENXIO)
			lart_once();
		goto out_new;
	}

	if (clone_flags & EVL_CLONE_PUBLIC) {
		ret = asprintf(&edevname, "/dev/evl/%s/%s", type, name);
		if (ret < 0) {
			ret = -ENOMEM;
			goto out_new;
		}

		efd = open(edevname, O_RDWR);
		if (efd < 0) {
			ret = -errno;
			goto out_element;
		}
	} else {
		efd = clone.efd;
	}

	/*
	 * Owned elements have no representation in the /dev/evl
	 * hierarchy, and no file descriptor. On success creating
	 * them, return zero immediately.
	 */
	if (clone_flags & __EVL_CLONE_OWNED) {
		efd = 0;
	} else {
		ret = flip_fd_flags(efd, F_SETFD, O_CLOEXEC);
		if (ret)
			goto out_element;

		if (nonblock) {
			ret = flip_fd_flags(efd, F_SETFL, O_NONBLOCK);
			if (ret)
				goto out_element;
		}
	}

	if (eids)
		*eids = clone.eids;

	ret = efd;

out_element:
	if (edevname)
		free(edevname);
out_new:
	close(ffd);
out_factory:
	free(fdevname);

	return ret;
}

int evl_open_element_vargs(const char *type,
		const char *fmt, va_list ap)
{
	char *path, *name;
	int efd, ret;

	ret = vasprintf(&name, fmt, ap);
	if (ret < 0)
		return -ENOMEM;

	ret = asprintf(&path, "/dev/evl/%s/%s", type, name);
	free(name);
	if (ret < 0)
		return -ENOMEM;

	efd = open(path, O_RDWR);
	if (efd < 0) {
		ret = -errno;
		goto fail_open;
	}

	ret = fcntl(efd, F_GETFD, 0);
	if (ret < 0) {
		ret = -errno;
		goto fail;
	}

	ret = fcntl(efd, F_SETFD, ret | O_CLOEXEC);
	if (ret) {
		ret = -errno;
		goto fail;
	}

	free(path);

	return efd;

fail:
	close(efd);
fail_open:
	free(path);

	return ret;
}

int evl_open_element(const char *type, const char *fmt, ...)
{
	va_list ap;
	int efd;

	va_start(ap, fmt);
	efd = evl_open_element_vargs(type, fmt, ap);
	va_end(ap);

	return efd;
}

int evl_open_raw(const char *type)
{
	char *devname;
	int efd, ret;

	ret = asprintf(&devname, "/dev/evl/%s", type);
	if (ret < 0)
		return -ENOMEM;

	efd = open(devname, O_RDWR);
	if (efd < 0) {
		ret = -errno;
		goto fail;
	}

	ret = flip_fd_flags(efd, F_SETFD, O_CLOEXEC);
	if (ret)
		goto fail_setfd;

	free(devname);

	return efd;

fail_setfd:
	close(efd);
fail:
	free(devname);

	return ret;
}
