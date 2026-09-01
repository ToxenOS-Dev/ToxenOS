#ifndef HEAP64_H
#define HEAP64_H

#include <stdint.h>

// Milestone 22: 64-bit kernel heap (kmalloc/kfree). See kernel/heap64.c
// for the full design writeup.
//
// Depends only on physmem64.h's public page allocation functions
// (physmem64_alloc_page/alloc_pages and their free counterparts) and
// physmem64_to_virt() -- never on physmem64's internal region/bitmap
// layout. Milestone 23 replaced physmem64's original fixed-256KB-pool
// implementation with a real Multiboot2-memory-map-driven allocator
// without requiring any change here, and also added
// physmem64_alloc_pages()/free_pages() (a real contiguous-run
// primitive), which this heap's span growth now uses instead of the
// original "hope single-page allocations land contiguous" workaround.

void heap64_init(void);

void* kmalloc(uint64_t size);
void  kfree(void* ptr);

// Aligned allocation/free -- counterpart pair, in the spirit of the
// 32-bit kernel's kmalloc_aligned/kfree_aligned (kernel/mm.c). `align`
// must be a nonzero power of two. Plain kmalloc() only guarantees
// 8-byte alignment (see kernel/heap64.c) -- anything stricter must go
// through this pair.
void* kmalloc_aligned(uint64_t size, uint64_t align);
void  kfree_aligned(void* ptr);

// ── Diagnostics ──────────────────────────────────────────────────────
// All fields are computed by a fresh walk of every live span/block at
// call time (not maintained incrementally), so they double as a sanity
// check in their own right -- garbage output here reflects real heap
// state, not a stale counter.
typedef struct {
    uint64_t total_bytes;        // sum of every span's size (all pages currently held from physmem64)
    uint64_t used_bytes;         // sum of payload bytes in USED blocks
    uint64_t free_bytes;         // sum of payload bytes in FREE blocks
    uint64_t overhead_bytes;     // header+footer bytes across every block, used and free
    uint32_t span_count;         // number of distinct physically-backed regions currently held
    uint32_t block_count;        // total blocks (used + free) across all spans
    uint32_t free_block_count;
    uint64_t largest_free_block; // payload bytes of the single biggest free block
} heap64_stats_t;

void heap64_stats(heap64_stats_t* out);

// Logs one line per live span and one line per block within it (address,
// size, USED/FREE) via klog(). Verbose -- for interactive debugging
// only, never called on the normal boot path.
void heap64_dump(void);

// Walks every live span/block and validates every header magic value,
// every header/footer size-and-magic pair, and that each span's blocks
// exactly tile it (no gap, no overlap, no block claiming a size that
// runs past the span). Returns 0 if the whole heap is structurally
// clean, or the address of the offending block header otherwise (never
// 0 on failure -- 0 is not a valid heap block address). Logs details of
// any failure via klog() either way.
uint64_t heap64_check(void);

// Runs the Milestone 22 self-test suite (basic alloc/free, reuse,
// splitting, coalescing + span release, fragmentation + recovery,
// kmalloc_aligned, multi-page allocation, repeated alloc/free cycles,
// double-free protection). Logs each case's result and a final
// pass/fail tally via klog(). Must be run on an otherwise-idle heap
// (nothing else allocated) for its span-count assertions to hold, and
// leaves the heap fully released (zero spans, all pages returned to
// physmem64) when every case passes. Returns 1 if every case passed.
int heap64_selftest(void);

#endif // HEAP64_H
