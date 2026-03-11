#ifndef MM_H
#define MM_H

#include <stdint.h>
#include <stddef.h>

void mm_init();
void* kmalloc(uint32_t size);
void kfree(void* ptr);
uint32_t mm_used();
uint32_t mm_free();

#endif