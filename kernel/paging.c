// ToxenOS/kernel/paging.c
#include <stdint.h>
#include "../include/paging.h"
#include "../include/memmap.h"
#include "../include/mm.h"
#include "../include/pmm.h"

uint32_t kernel_directory[1024] __attribute__((aligned(4096)));

// ── paging_init ───────────────────────────────────────────────────────────────
// Called after pmm_init() and mm_init().
// Rebuilds the page tables properly:
//   - Kernel is mapped at 0xC0000000..0xC0400000 (first 4MB, no PAGE_USER)
//   - User space (0x00000000..0xBFFFFFFF) starts unmapped
//   - Identity map from boot.asm is already removed before we get here
//
// We use static kernel_tables arrays here so the tables themselves are in
// kernel BSS (virtual addresses), not allocated from the heap.  This avoids
// a chicken-and-egg problem where the heap isn't set up yet.
static uint32_t kernel_tables[256][1024] __attribute__((aligned(4096)));

void paging_init()
{
    // Clear the directory
    for (int i = 0; i < 1024; i++)
        kernel_directory[i] = 0;

    // Map the kernel: virtual 0xC0000000..0xC0400000 -> physical 0x00000000..0x00400000
    // 256 page tables cover 256 * 4MB = 1GB, starting at directory entry 768
    // (0xC0000000 >> 22 = 768).
    // We only need the first few tables to cover the kernel image + heap,
    // but map the full 256 (1GB kernel space) for simplicity — unused entries
    // stay zeroed (not present), so there's no overhead beyond the table storage.
    for (int table = 0; table < 256; table++)
    {
        for (int page = 0; page < 1024; page++)
        {
            uint32_t phys = (uint32_t)(table * 1024 + page) * PAGE_SIZE;
            // Kernel pages: PRESENT + WRITABLE, no PAGE_USER
            kernel_tables[table][page] = phys | PAGE_PRESENT | PAGE_WRITABLE;
        }
        // Physical address of the table (it's in BSS, virtual addr is known)
        uint32_t table_phys = KVIRT_TO_PHYS((uint32_t)kernel_tables[table]);
        kernel_directory[768 + table] = table_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    paging_switch(kernel_directory);
}

// ── paging_create_directory ───────────────────────────────────────────────────
// Creates a new page directory for a user process.
// Copies kernel entries (768..1023) so kernel is accessible in every process
// (needed for syscalls/interrupts which run in ring 0 but in the process context).
// User entries (0..767) start empty.
uint32_t* paging_create_directory()
{
    uint32_t* dir = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!dir) return 0;

    // Clear user space entries
    for (int i = 0; i < 768; i++)
        dir[i] = 0;

    // Share kernel page tables
    for (int i = 768; i < 1024; i++)
        dir[i] = kernel_directory[i];

    return dir;
}

// ── paging_alloc_page / paging_free_page ─────────────────────────────────────
// Allocate / free physical frames via the PMM.
// Since we're higher-half, phys addr != virt addr for kernel data.
// BUT: phys_alloc_page returns a physical address, and we access the page
// via identity... wait, we don't have identity map anymore.
//
// Problem: after paging_init removes PAGE_USER from kernel entries and
// sets up the proper tables, physical addresses 0x00000000..0x003FFFFF are
// mapped at KERNEL_VIRT_BASE (0xC0000000).  So to access physical address P,
// we use virtual address P + KERNEL_VIRT_BASE.
//
// paging_alloc_page returns the PHYSICAL address (what goes in page table entries).
// Callers that need to write to the page use KPHYS_TO_VIRT(phys).

uint32_t paging_alloc_page()
{
    return phys_alloc_page();
}

void paging_free_page(uint32_t phys_addr)
{
    phys_free_page(phys_addr);
}

// Free a heap-allocated page table or directory.
void paging_free_aligned(void* ptr)
{
    kfree_aligned(ptr);
}

// ── paging_map ────────────────────────────────────────────────────────────────
void paging_map(uint32_t* directory, uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(directory[dir_idx] & PAGE_PRESENT))
    {
        // Allocate a new page table from the heap
        uint32_t* table = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!table) return;
        for (int i = 0; i < 1024; i++) table[i] = 0;
        // Store physical address in the directory
        uint32_t table_phys = KVIRT_TO_PHYS((uint32_t)table);
        directory[dir_idx] = table_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    else if (directory != kernel_directory &&
             (directory[dir_idx] & ~0xFFF) == (kernel_directory[dir_idx] & ~0xFFF))
    {
        // Shared kernel table — clone before adding user mappings
        uint32_t  old_phys  = directory[dir_idx] & ~0xFFF;
        uint32_t* old_table = (uint32_t*)KPHYS_TO_VIRT(old_phys);
        uint32_t* new_table = (uint32_t*)kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        if (!new_table) return;
        for (int i = 0; i < 1024; i++) new_table[i] = old_table[i];
        uint32_t new_phys = KVIRT_TO_PHYS((uint32_t)new_table);
        directory[dir_idx] = new_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint32_t  table_phys = directory[dir_idx] & ~0xFFF;
    uint32_t* table      = (uint32_t*)KPHYS_TO_VIRT(table_phys);
    table[table_idx]     = phys | PAGE_PRESENT | flags;
}

// ── paging_unmap ─────────────────────────────────────────────────────────────
void paging_unmap(uint32_t* directory, uint32_t virt)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;
    if (!(directory[dir_idx] & PAGE_PRESENT)) return;
    uint32_t  table_phys = directory[dir_idx] & ~0xFFF;
    uint32_t* table      = (uint32_t*)KPHYS_TO_VIRT(table_phys);
    table[table_idx]     = 0;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

// ── paging_switch ─────────────────────────────────────────────────────────────
void paging_switch(uint32_t* directory)
{
    // cr3 needs the PHYSICAL address of the directory
    uint32_t phys = KVIRT_TO_PHYS((uint32_t)directory);
    __asm__ volatile(
        "mov %0, %%cr3\n"
        :
        : "r"(phys)
        :
    );
}
