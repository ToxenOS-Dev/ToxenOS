#ifndef PHYSMEM64_H
#define PHYSMEM64_H

#include <stdint.h>

// Milestone 23: real Multiboot2-memory-map-driven physical memory
// manager. Replaces Milestone 8's fixed 64-page/256KB static pool.
// See kernel/physmem64.c for the full design writeup (region layout,
// reservation handling, direct-map bootstrap).
//
// Public interface is deliberately unchanged from Milestone 8 for
// physmem64_alloc_page()/physmem64_free_page() -- Milestone 22's
// kernel/heap64.c keeps working against these two functions with no
// changes required. physmem64_init()'s signature necessarily changes
// (it now needs to see the Multiboot2 info pointer and the framebuffer
// range to build reservations from them); its one call site
// (kernel/kernel64.c) is updated accordingly.

// mb_info_addr: physical address of the Multiboot2 info block (as
// passed to kernel_main64) -- parsed for the memory map tag (type 6).
// fb_addr/fb_size: the framebuffer LFB range if one was found (0/0 if
// not), reserved defensively even though it is normally already outside
// any Multiboot2 "available" range.
void physmem64_init(uint64_t mb_info_addr, uint64_t fb_addr, uint64_t fb_size);

// Returns the physical address of one freshly zeroed 4KB page, or 0 if
// every managed region is exhausted. Equivalent to physmem64_alloc_pages(1).
uint64_t physmem64_alloc_page(void);
// Frees a page previously returned by physmem64_alloc_page/alloc_pages(_,1).
// Invalid, out-of-region, or double-free addresses are logged and
// ignored, not fatal. Equivalent to physmem64_free_pages(phys, 1).
void     physmem64_free_page(uint64_t phys);

// Milestone 23: allocates `count` PHYSICALLY CONTIGUOUS, freshly zeroed
// 4KB pages (never spanning two regions, since regions are not
// contiguous with each other by definition) and returns the physical
// address of the first one, or 0 if no single region currently has a
// large enough free run. This is the primitive kernel/heap64.c's
// multi-page span growth was missing at Milestone 22.
uint64_t physmem64_alloc_pages(uint64_t count);
// Frees `count` contiguous pages previously returned together by one
// physmem64_alloc_pages() call. Must describe exactly one prior
// allocation (or a subset that was never itself freed) -- if ANY page
// in the range is not currently allocated, the whole call is rejected
// (logged, ignored) rather than partially freeing it.
void     physmem64_free_pages(uint64_t phys, uint64_t count);

// Turns a physical address returned by this allocator into a
// dereferenceable kernel pointer, and back. Backed by a dedicated
// direct-map window distinct from the kernel image's own high-half
// mapping (memmap64.h's phys_to_ptr()/phys_of(), which remain for
// kernel-image-symbol use only -- e.g. kernel/ring3_test64.c, never
// modified). NOT valid for arbitrary physical addresses outside what
// this allocator manages (MMIO, unmanaged holes, etc.).
void*    physmem64_to_virt(uint64_t phys);
uint64_t physmem64_to_phys(const void* virt);

// Milestone 28: maps a physical DEVICE MMIO range (NOT RAM this
// allocator owns -- a PCI BAR target, typically) into the SAME direct-
// map window physmem64_to_virt() covers for RAM, but with cache-disable
// set on its page-table entries, appropriate for device registers
// (unlike RAM, where a cached stale read/write of an MMIO doorbell or
// status register would be a correctness bug, not just a performance
// one). Returns a kernel virtual pointer usable immediately, or NULL on
// failure (direct-map/bootstrap-pool exhaustion). Idempotent per
// 2MB-aligned chunk. Never allocates from or frees back to the page
// allocator -- the range is owned by hardware, not by physmem64.
//
// MUST be called before the first process64_spawn() -- paging64_create_
// as() copies the shared pdpt_high[] direct-map entries into every new
// process's own page tables BY VALUE at creation time, not by
// reference, so a process created before a given MMIO mapping exists
// would never see it. In practice this means all PCI/storage-driver
// probing must happen during kernel64.c's boot sequence, before
// init64.nex64 (or any other process) is spawned -- exactly where
// physmem64_init/ata64_init/txfs64_mount already run today.
//
// Limitation: if the requested range falls within a 2MB chunk ALREADY
// mapped for something else (e.g. overlapping a RAM region this
// allocator manages), the existing entry's cache attributes are left
// unchanged rather than upgraded to cache-disabled -- real hardware
// could theoretically place an MMIO BAR close enough to RAM to share a
// 2MB-aligned chunk, but this does not happen on any QEMU machine type
// this kernel targets. Documented, not solved, this milestone (would
// require per-4KB-page, not per-2MB-chunk, ownership tracking).
void* physmem64_map_mmio(uint64_t phys, uint64_t size);

// Diagnostic-only: is every byte of [phys, phys+len) currently part of
// a region this allocator manages (whether that memory happens to be
// free or allocated right now)? Returns 0 for anything permanently
// reserved (kernel image, boot structures, framebuffer, unmanaged
// holes) or outside all detected RAM. Useful for verifying reservation
// correctness; not needed on any normal allocation path.
int physmem64_range_is_allocatable(uint64_t phys, uint64_t len);

typedef struct {
    uint64_t usable_ram_bytes;      // sum of Multiboot2 type=1 entries, before our own reservations
    uint64_t reserved_bytes;        // usable_ram_bytes - bytes actually left in managed regions
    uint64_t kernel_image_bytes;    // low memory + kernel image reservation size
    uint64_t mb_info_bytes;         // Multiboot2 info block reservation size
    uint64_t framebuffer_bytes;     // LFB reservation size (0 if no framebuffer)
    uint64_t managed_pages;         // total pages actually available to the allocator
    uint64_t free_pages;
    uint64_t used_pages;
    uint32_t region_count;
    uint64_t largest_region_pages;
    uint64_t largest_free_run_pages; // biggest single contiguous free run, across all regions
} physmem64_stats_t;

void physmem64_stats(physmem64_stats_t* out);
// Logs one line per managed region (base, page count, free count,
// pages spent on its own bitmap) via klog(). Verbose -- for interactive
// debugging only, never called on the normal boot path.
void physmem64_dump(void);

// Runs the Milestone 23 self-test suite (single-page alloc/free, reuse,
// contiguous multi-page alloc/free, safe exhaustion and recovery,
// reservation correctness, bookkeeping consistency). Logs each case's
// result and a final pass/fail tally via klog(). Returns 1 if every
// case passed. Restores the allocator to its pre-test state on a full
// pass (every page it borrowed is freed again).
int physmem64_selftest(void);

#endif // PHYSMEM64_H
