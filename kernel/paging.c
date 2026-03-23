// ToxenOS/kernel/paging.c
#include <stdint.h>
#include "../include/paging.h"
#include "../include/mm.h"

uint32_t kernel_directory[1024] __attribute__((aligned(4096)));
static uint32_t kernel_tables[256][1024] __attribute__((aligned(4096)));

void paging_init()
{
    for (int i = 0; i < 1024; i++)
        kernel_directory[i] = 0;

    // Identity-map first 1GB — same as original working version
    // PAGE_USER on both directory AND table entries so user processes
    // can have pages mapped anywhere in the first 1GB
    for (int table = 0; table < 256; table++)
    {
        for (int page = 0; page < 1024; page++)
        {
            uint32_t phys = (table * 1024 + page) * PAGE_SIZE;
            kernel_tables[table][page] = phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        }
        kernel_directory[table] = (uint32_t)kernel_tables[table]
                                 | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    paging_switch(kernel_directory);
}

uint32_t* paging_create_directory()
{
    uint32_t* dir = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!dir) return 0;

    // Copy entire kernel directory — user gets same mappings as kernel
    // Individual PTEs control what user can actually access
    for (int i = 0; i < 1024; i++)
        dir[i] = kernel_directory[i];

    return dir;
}

uint32_t paging_alloc_page()
{
    void* p = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!p) return 0;
    uint32_t* pg = (uint32_t*)p;
    for (int i = 0; i < 1024; i++) pg[i] = 0;
    return (uint32_t)p;
}

void paging_map(uint32_t* directory, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(directory[dir_idx] & PAGE_PRESENT))
    {
        uint32_t* table = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!table) return;
        for (int i = 0; i < 1024; i++) table[i] = 0;
        directory[dir_idx] = (uint32_t)table | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint32_t* table = (uint32_t*)(directory[dir_idx] & ~0xFFF);
    table[table_idx] = phys | PAGE_PRESENT | flags;
}

void paging_unmap(uint32_t* directory, uint32_t virt)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;
    if (!(directory[dir_idx] & PAGE_PRESENT)) return;
    uint32_t* table = (uint32_t*)(directory[dir_idx] & ~0xFFF);
    table[table_idx] = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void paging_switch(uint32_t* directory)
{
    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        :
        : "r"(directory)
        : "eax"
    );
}
