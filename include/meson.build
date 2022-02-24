# SPDX-License-Identifier: MIT

uapi_opt = get_option('uapi')
o_uapi = meson.current_build_dir() / 'uapi'
uapi_env = environment()
uapi_env.set('ARCH', libevl_arch)
uapi_env.set('UAPI', uapi_opt)
uapi_env.set('O_UAPI', o_uapi)
run_command(libevl_scripts / 'setup-uapi.sh',
    check : true,
    env : uapi_env
)

arch_incdir = '../lib/arch' / libevl_arch / 'include'
libevl_incdirs = include_directories(
    '.',
    arch_incdir,
    meson.current_build_dir()
)

libevl_headers = [
    'evl/atomic.h',
    'evl/clock.h',
    'evl/compiler.h',
    'evl/event.h',
    'evl/evl.h',
    'evl/flags.h',
    'evl/heap.h',
    'evl/list.h',
    'evl/mutex.h',
    'evl/observable.h',
    'evl/poll.h',
    'evl/proxy.h',
    'evl/ring_ptr.h',
    'evl/sched.h',
    'evl/sem.h',
    'evl/syscall.h',
    'evl/thread.h',
    'evl/timer.h',
    'evl/xbuf.h',
]

install_headers(
    libevl_headers,
    subdir : 'evl'
)

libeshi_headers = [
    'evl/atomic.h',
    'evl/heap.h',
    'evl/list.h',
    'eshi/evl/clock.h',
    'eshi/evl/event.h',
    'eshi/evl/evl.h',
    'eshi/evl/flags.h',
    'eshi/evl/mutex.h',
    'eshi/evl/poll.h',
    'eshi/evl/proxy.h',
    'eshi/evl/sched.h',
    'eshi/evl/sem.h',
    'eshi/evl/syscall.h',
    'eshi/evl/thread.h',
    'eshi/evl/timer.h',
    'eshi/evl/uapi.h',
]

install_headers(
    libeshi_headers,
    subdir : 'eshi'
)

uapi_install = find_program('copy-uapi.sh',
    dirs : libevl_scripts,
    required : true
)
meson.add_install_script(uapi_install, o_uapi, get_option('prefix') / get_option('includedir'))
