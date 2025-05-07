#ifndef _EVL_LATMUS_GPIO_H
#define _EVL_LATMUS_GPIO_H

#include "latency.h"

void setup_measurement_on_gpio(bool oob_mode);

void setup_gpio_pins(int *fds);

void *gpio_responder_thread(void *arg);

int parse_gpio_spec(const char *spec, int *pin,
		int *hdflags, int *evflags);

void find_latmon_ip(const char *host);

extern int gpio_infd, gpio_outfd;

extern int gpio_inpin, gpio_outpin;

extern int gpio_hdinflags,
	gpio_hdoutflags;

extern int gpio_evinflags;

#endif /* !_EVL_LATMUS_GPIO_H */
