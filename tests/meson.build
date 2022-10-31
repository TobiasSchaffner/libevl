# SPDX-License-Identifier: MIT

libtest = static_library('test_helpers', 'helpers.c',
    include_directories : [ libevl_incdirs ],
)
libtest_dep = declare_dependency(link_with : libtest)

libtest_eshi = static_library('test_helpers_eshi', 'helpers.c',
    include_directories : [ libeshi_incdir ],
)
libtest_eshi_dep = declare_dependency(link_with : libtest_eshi)

test_programs = [
    'basic-xbuf',
    'clock-timer-periodic',
    'clone-fork-exec',
    'detach-self',
    'duplicate-element',
    'element-visibility',
    'fpu-preload',
    'fpu-stress',
    'heap-torture',
    'mapfd',
    'monitor-deadlock',
    'monitor-event',
    'monitor-flags',
    'monitor-pi',
    'monitor-pi-deadlock',
    'monitor-pi-stress',
    'monitor-pi-deboost',
    'monitor-pp-dynamic',
    'monitor-pp-lower',
    'monitor-pp-nested',
    'monitor-pp-pi',
    'monitor-pp-raise',
    'monitor-pp-tryenter',
    'monitor-pp-weak',
    'monitor-steal',
    'monitor-trylock',
    'monitor-wait-multiple',
    'monitor-wait-requeue',
    'observable-hm',
    'observable-inband',
    'observable-unicast',
    'observable-onchange',
    'observable-oob',
    'observable-race',
    'observable-thread',
    'poll-close',
    'poll-flags',
    'poll-many',
    'poll-multiple',
    'poll-nested',
    'poll-observable-inband',
    'poll-observable-oob',
    'poll-sem',
    'poll-xbuf',
    'proxy-echo',
    'proxy-eventfd',
    'proxy-pipe',
    'proxy-poll',
    'sched-quota-accuracy',
    'sched-tp-accuracy',
    'sem-close-unblock',
    'sem-timedwait',
    'sem-wait',
    'simple-clone',
    'stax-lock',
    'stax-warn',
    'thread-mode-bits',
]

foreach t : test_programs
    x = executable(t, t + '.c',
    install : true,
    install_dir : 'tests',
    dependencies : [ libtest_dep, libevl_dep ]
)
endforeach

test_programs_with_m = [
    'fault',
]

m_dep = cc.find_library('m', required : true)
foreach t : test_programs_with_m
    x = executable(t, t + '.c',
    c_args : '-Wno-array-bounds',
    install : true,
    install_dir : 'tests',
    dependencies : [ libtest_dep, libevl_dep, m_dep ]
)
endforeach

test_programs_with_atomic = [
    'ring-spray',
]

atomic_dep = cc.find_library('atomic', required : true)
foreach t : test_programs_with_atomic
    x = executable(t, t + '.c',
    install : true,
    install_dir : 'tests',
    dependencies : [ libtest_dep, libevl_dep, atomic_dep ]
)
endforeach

test_eshi_programs = [
    'clock-timer-periodic',
    'detach-self',
    'heap-torture',
    'monitor-event',
    'monitor-wait-multiple',
    'monitor-flags',
    'monitor-trylock',
    'poll-nested',
    'poll-sem',
    'proxy-eventfd',
    'proxy-pipe',
    'sem-timedwait',
    'sem-wait',
]

foreach t : test_eshi_programs
    x = executable(t + '.eshi', t + '.c',
    c_args : '-D__ESHI__',
    install : true,
    install_dir : 'tests/eshi',
    dependencies : [ libtest_eshi_dep, libeshi_dep ]
)
endforeach

subdir('compile-tests')
