#include <stdint.h>
#include "../include/paging.h"
#include "../include/mm.h"

// kernel page directory — aligned to 4KB
uint32_t kernel_directory[1024] __attribute__((aligned(4096)));
static uint32_t kernel_tables[256][1024] __attribute__((aligned(4096)));

void paging_init()
{
    // clear directory
    for (int i = 0; i < 1024; i++)
        kernel_directory[i] = 0;

    // identity map first 256 page tables (256 * 4MB = first 1GB)
    // this maps virtual == physical for all kernel memory
    for (int table = 0; table < 256; table++)
    {
        for (int page = 0; page < 1024; page++)
        {
            uint32_t phys = (table * 1024 + page) * PAGE_SIZE;
            kernel_tables[table][page] = phys | PAGE_PRESENT | PAGE_WRITABLE;
        }

        kernel_directory[table] = (uint32_t)kernel_tables[table]
                                 | PAGE_PRESENT | PAGE_WRITABLE;
    }

    paging_switch(kernel_directory);
}

uint32_t* paging_create_directory()
{
    uint32_t* dir = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);

    for (int i = 0; i < 1024; i++)
        dir[i] = kernel_directory[i];

    return dir;
}

void paging_map(uint32_t* directory, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(directory[dir_idx] & PAGE_PRESENT))
    {
        uint32_t* table = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        for (int i = 0; i < 1024; i++)
            table[i] = 0;
        directory[dir_idx] = (uint32_t)table | PAGE_PRESENT | PAGE_WRITABLE | flags;
    }

    uint32_t* table = (uint32_t*)(directory[dir_idx] & ~0xFFF);
    table[table_idx] = phys | PAGE_PRESENT | flags;
}

void paging_switch(uint32_t* directory)
{
    __asm__ volatile(
        "mov %0, %%cr3\n"       // load page directory
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"  // set PG bit
        "mov %%eax, %%cr0\n"    // enable paging
        :
        : "r"(directory)
        : "eax"
    );
}