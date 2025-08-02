# SPDX-License-Identifier: MIT

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
    c_args: [ '-DLATMON_NET_PORT=' + get_option('latmon-port').to_string() ],
    install: true,
    dependencies: libevl_dep
)
