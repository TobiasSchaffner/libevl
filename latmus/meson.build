# SPDX-License-Identifier: MIT

latmon_incdir = include_directories(['.', 'zephyr/latmon/src'])

latmus_sources = [
       'gpio.c',
       'latmus.c',
       'net.c',
       'stats.c',
       'timer.c',
       'tuning.c',
]

executable('latmus',
    latmus_sources,
    install: true,
    include_directories : latmon_incdir,
    dependencies: libevl_dep
)
