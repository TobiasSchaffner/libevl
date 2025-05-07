# SPDX-License-Identifier: MIT

latmon_incdir = include_directories(['.', 'zephyr/latmon/src'])

latmus_sources = [
       'latmus.c',
       'latency.c',
       'tuning.c',
       'timer.c',
       'gpio.c',
]

executable('latmus',
    latmus_sources,
    install: true,
    include_directories : latmon_incdir,
    dependencies: libevl_dep
)
