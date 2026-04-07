#ifndef MM_H
#define MM_H

#include <stdint.h>
#include <stddef.h>

void mm_init();
void* kmalloc_aligned(uint32_t size, uint32_t align);
void  kfree_aligned(void* ptr);   // counterpart to kmalloc_aligned
void* kmalloc(uint32_t size);
void  kfree(void* ptr);
uint32_t mm_used();
uint32_t mm_free();

#endif