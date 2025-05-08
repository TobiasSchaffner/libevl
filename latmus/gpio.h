 /*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2020 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_LATMUS_GPIO_H
#define _EVL_LATMUS_GPIO_H

#include <stdbool.h>
#include <sys/types.h>

void run_gpio_test(bool oob_mode, size_t histogram_cells);

void find_latmon_ip(const char *host);

#endif /* !_EVL_LATMUS_GPIO_H */
