#ifndef TIMER64_H
#define TIMER64_H
#include <stdint.h>
// M+11C: the timer's optional poll hook. NULL whenever no queue is POLL-owned, so
// a device in IRQ mode costs the timer exactly one pointer test and reads no ring.
void timer64_set_poll_hook(void (*fn)(void));
void (*timer64_get_poll_hook(void))(void);   // NULL while no queue is POLL-owned (self-test / diagnostics)
uint64_t timer64_get_ticks(void);
#endif
