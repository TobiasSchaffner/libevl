# SPDX-License-Identifier: MIT

libeshi_sources = [
    'clock.c',
    'event.c',
    'flags.c',
    'heap.c',
    'init.c',
    'mutex.c',
    'poll.c',
    'proxy.c',
    'sched.c',
    'sem.c',
    'thread.c',
    'timer.c',
    'version.c',
]

stamp_generator = find_program('generate-git-stamp.sh',
    dirs : libevl_scripts,
    required : true
)

gitdir = meson.project_source_root() / '.git'
git_stamp_h = custom_target(
    'git_stamp.h',
    output : 'git_stamp.h',
    build_always_stale : true,
    command : [ stamp_generator, gitdir , '@OUTPUT@' ],
)
libeshi_sources += git_stamp_h
libeshi_incdir = include_directories('../include/eshi')

libeshi = both_libraries('eshi', libeshi_sources,
     include_directories : [ libeshi_incdir, libevl_incdirs ],
     c_args : [ '-DLIBVERSION="' + libevl_version + '"' ],
     install : true,
     version : libeshi_dso_version,
     dependencies : [ pthread_dep, rt_dep ],
)

libeshi_dep = declare_dependency(link_with : libeshi,
  include_directories : [ libeshi_incdir, libevl_incdirs ],
  dependencies : [ pthread_dep, rt_dep ],
)
