// ToxenOS/kernel/paging.c
#include <stdint.h>
#include "../include/paging.h"
#include "../include/mm.h"
#include "../include/pmm.h"

uint32_t kernel_directory[1024] __attribute__((aligned(4096)));
static uint32_t kernel_tables[256][1024] __attribute__((aligned(4096)));

void paging_init()
{
    for (int i = 0; i < 1024; i++)
        kernel_directory[i] = 0;

    // Identity-map first 1GB.
    // NOTE: PAGE_USER is still set here — the kernel/user split (removing
    // PAGE_USER from kernel mappings) is Phase 2 task 2, done after the
    // PMM is solid.  Removing it now without the full high-half kernel
    // setup would break boot.
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
    // Allocate the directory itself from the kernel heap (it needs to be
    // accessible by virtual address for us to write into it).
    uint32_t* dir = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!dir) return 0;

    for (int i = 0; i < 1024; i++)
        dir[i] = kernel_directory[i];

    return dir;
}

// Allocate one physical page frame via the PMM.
// Since we're still identity-mapped, phys == virt and we can access it directly.
uint32_t paging_alloc_page()
{
    return phys_alloc_page();
}

// Free a physical frame back to the PMM.
void paging_free_page(uint32_t phys_addr)
{
    phys_free_page(phys_addr);
}

// Free a page-aligned allocation made by kmalloc_aligned (page tables,
// page directories).  These come from the kernel heap, not the PMM.
void paging_free_aligned(void* ptr)
{
    kfree_aligned(ptr);
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
    else if (directory == kernel_directory)
    {
        // Mapping into kernel directory directly — use as-is
    }
    else if ((directory[dir_idx] & ~0xFFF) == (kernel_directory[dir_idx] & ~0xFFF))
    {
        // Shared kernel page table — clone before adding user mappings
        uint32_t* old_table = (uint32_t*)(directory[dir_idx] & ~0xFFF);
        uint32_t* new_table = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!new_table) return;
        for (int i = 0; i < 1024; i++) new_table[i] = old_table[i];
        directory[dir_idx] = (uint32_t)new_table | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
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
