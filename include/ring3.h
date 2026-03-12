#ifndef RING3_H
#define RING3_H

#include <stdint.h>

void jump_to_ring3(void (*entry)(), uint32_t user_stack);

#endif