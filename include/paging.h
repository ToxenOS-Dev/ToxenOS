#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE       4096
#define PAGE_PRESENT    0x1
#define PAGE_WRITABLE   0x2
#define PAGE_USER       0x4

extern uint32_t kernel_directory[];

void paging_init();
uint32_t* paging_create_directory();
void paging_map(uint32_t* directory, uint32_t virt, uint32_t phys, uint32_t flags);
void paging_switch(uint32_t* directory);

#endif