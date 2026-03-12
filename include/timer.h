#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

int timer_schedule_pending();

void timer_init(uint32_t frequency);
uint32_t timer_getticks();

#endif