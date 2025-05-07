#ifndef _EVL_LATMUS_TIMER_H
#define _EVL_LATMUS_TIMER_H

#include "latency.h"

void setup_measurement_on_timer(void);

void *timer_responder(void *arg);

void *timer_test_sitter(void *arg);

#endif /* !_EVL_LATMUS_TIMER_H */
