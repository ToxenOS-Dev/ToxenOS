#ifndef PHYSMEM64_H
#define PHYSMEM64_H

#include <stdint.h>

// Milestone 8: a tiny fixed-pool physical frame allocator. Deliberately
// NOT a general physical-memory manager -- there is no multiboot2 mmap
// parsing here, no support for physical memory outside this pool. The
// pool lives in kernel BSS, which is itself inside the boot-time
// flat-mapped first 64MB of physical RAM (kernel/boot64.asm's pd[0..31]),
// so every page handed out by this allocator is already reachable via
// the existing identity map AND high-half map with zero new mapping
// infrastructure -- no per-page-table-page special-casing needed.
//
// 64 pages (256KB) covers one process's worst case (4 page-table pages +
// up to 16 segment pages + 1 stack page = ~21) with headroom, since user
// processes remain strictly serialized (create -> load -> run -> exit ->
// free -> next) -- never more than one process's pages live at once.
// Real physical memory management (multiboot mmap-driven, spanning all of
// RAM) is future work for whenever something actually needs more than
// this fixed pool.
#define PHYSMEM64_POOL_PAGES 64

void     physmem64_init(void);
// Returns the physical address of a freshly zeroed 4KB page, or 0 if the
// pool is exhausted.
uint64_t physmem64_alloc_page(void);
// Frees a page previously returned by physmem64_alloc_page. Double-frees
// and out-of-pool addresses are logged and ignored, not fatal.
void     physmem64_free_page(uint64_t phys);

#endif // PHYSMEM64_H
