#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096
#define PAGE_PRESENT    0x1
#define PAGE_WRITABLE   0x2
#define PAGE_USER       0x4

// User stack virtual address — top of stack, grows down from here
#define USER_STACK_TOP   0xC0000000   // 3GB mark
#define USER_STACK_PAGES 4            // 4 pages = 16KB stack per process

extern uint32_t kernel_directory[1024];

void      paging_init();
uint32_t* paging_create_directory();
uint32_t  paging_alloc_page();
void      paging_map(uint32_t* dir, uint32_t virt, uint32_t phys, uint32_t flags);
void      paging_unmap(uint32_t* dir, uint32_t virt);
void      paging_switch(uint32_t* directory);

#endif
