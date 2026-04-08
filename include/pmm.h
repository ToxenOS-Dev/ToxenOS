#ifndef PMM_H
#define PMM_H

// pmm.h — physical page frame allocator
//
// Tracks all physical RAM using a bitmap (1 bit per 4KB frame).
// Must be initialised from the multiboot2 memory map before paging_init().
//
// After pmm_init(), use phys_alloc_page() / phys_free_page() for all
// physical frame allocation.  paging_alloc_page() wraps these.
//
// The kernel heap (kmalloc) continues to live in a fixed 16MB region above
// kernel_end — it does NOT go through the PMM.  The PMM manages everything
// outside that region for use as page frames.

#include <stdint.h>

// Maximum physical memory we can track: 4GB / 4KB = 1M frames = 128KB bitmap
#define PMM_MAX_FRAMES    (1024 * 1024)
#define PMM_FRAME_SIZE    4096

// Initialise the PMM from the multiboot2 info block.
// Must be called before paging_init() and before any phys_alloc_page() calls.
// mb_info_addr: physical address of the multiboot2 info structure.
void     pmm_init(uint32_t mb_info_addr);

// Allocate one physical 4KB frame.  Returns the physical address, or 0 on OOM.
uint32_t phys_alloc_page(void);

// Free a physical frame previously returned by phys_alloc_page().
void     phys_free_page(uint32_t phys_addr);

// Diagnostic: total / free frame counts.
uint32_t pmm_total_frames(void);
uint32_t pmm_free_frames(void);

#endif // PMM_H
