#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096
#define PAGE_PRESENT    0x1
#define PAGE_WRITABLE   0x2
#define PAGE_USER       0x4

// User address space layout constants live in memmap.h.
// Include it wherever you need USER_STACK_TOP, USER_ELF_BASE, etc.

extern uint32_t kernel_directory[1024];

void      paging_init();
uint32_t* paging_create_directory();
uint32_t  paging_alloc_page();
void      paging_free_aligned(void* ptr);
void      paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);
void      paging_unmap(uint32_t* dir, uint32_t virt);
void      paging_switch(uint32_t* directory);

#endif
