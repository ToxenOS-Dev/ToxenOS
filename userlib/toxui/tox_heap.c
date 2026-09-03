// ToxenOS/userlib/toxui/tox_heap.c — see tox_heap.h's header comment.
#include <stdint.h>
#include "tox_heap.h"
#include "tox64.h"

// Grows the heap by at least this many bytes at a time (rather than
// exactly what one allocation needs) so a run of small allocations
// doesn't cost one sys_brk syscall each -- sys_brk itself already maps
// whole pages internally (see kernel/uservm64.c's uservm64_brk), so
// this is purely about syscall-count amortization on top of that.
#define TOX_HEAP_MIN_GROW (64u * 1024u)
#define TOX_ALIGN 16u

typedef struct tox_heap_block {
    struct tox_heap_block* next; // next block by ADDRESS (both free and used share this one chain)
    uint64_t size;               // payload bytes (excludes this header)
    int free;
} tox_heap_block_t;

#define BLOCK_HDR_SIZE (sizeof(tox_heap_block_t))

static tox_heap_block_t* g_first = 0;
static tox_heap_block_t* g_last  = 0; // highest-address block -- O(1) append target for heap growth

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + (a - 1)) & ~(a - 1); }

// Grows the process heap via sys_brk by enough to satisfy an
// allocation of at least `min_payload` bytes, extending the existing
// last block if it's already free (no new header needed) or appending
// a brand-new free block otherwise. The freshly grown region is always
// physically contiguous with whatever was already at the old break
// (sys_brk only ever grows upward), which is what makes treating it as
// "the next block in address order" correct without re-scanning
// anything.
static int grow_heap(uint64_t min_payload) {
    uint64_t want_extra = min_payload + BLOCK_HDR_SIZE;
    if (want_extra < TOX_HEAP_MIN_GROW) want_extra = TOX_HEAP_MIN_GROW;

    uint64_t cur = sys_brk(0); // query current break
    if (cur == (uint64_t)-1) return -1;
    uint64_t got = sys_brk(cur + want_extra);
    if (got == (uint64_t)-1 || got <= cur) return -1;
    uint64_t actual_grow = got - cur;

    if (g_last && g_last->free) {
        g_last->size += actual_grow;
    } else {
        tox_heap_block_t* nb = (tox_heap_block_t*)(uintptr_t)cur;
        nb->size = actual_grow - BLOCK_HDR_SIZE;
        nb->free = 1;
        nb->next = 0;
        if (g_last) g_last->next = nb; else g_first = nb;
        g_last = nb;
    }
    return 0;
}

void* tox_alloc(uint64_t size) {
    if (size == 0) return 0;
    size = align_up(size, TOX_ALIGN);

    tox_heap_block_t* b = g_first;
    while (b) {
        if (b->free && b->size >= size) break;
        b = b->next;
    }
    if (!b) {
        if (grow_heap(size) < 0) return 0;
        b = g_last; // grow_heap guarantees this is free and >= size (see its own contract)
    }

    // Split off the remainder if it's big enough to be worth its own
    // header + a minimal payload -- avoids leaving a genuinely useless
    // few-byte free sliver that could never satisfy any future alloc.
    if (b->size >= size + BLOCK_HDR_SIZE + TOX_ALIGN) {
        tox_heap_block_t* rem = (tox_heap_block_t*)((uint8_t*)b + BLOCK_HDR_SIZE + size);
        rem->size = b->size - size - BLOCK_HDR_SIZE;
        rem->free = 1;
        rem->next = b->next;
        b->next = rem;
        b->size = size;
        if (g_last == b) g_last = rem;
    }

    b->free = 0;
    return (uint8_t*)b + BLOCK_HDR_SIZE;
}

void tox_free(void* ptr) {
    if (!ptr) return;
    tox_heap_block_t* b = (tox_heap_block_t*)((uint8_t*)ptr - BLOCK_HDR_SIZE);
    b->free = 1;

    // Coalesce forward: b->next IS the physically-next block by
    // construction (every block is created either by splitting a
    // contiguous region or appending a contiguous freshly-grown one).
    if (b->next && b->next->free) {
        if (g_last == b->next) g_last = b;
        b->size += BLOCK_HDR_SIZE + b->next->size;
        b->next = b->next->next;
    }

    // Coalesce backward: this allocator's chain has no `prev` pointer
    // (kept deliberately simple -- see this file's header comment), so
    // finding the physical predecessor is a linear scan. Fine for the
    // moderate allocation counts a graphics/font library produces; not
    // fine for a general-purpose OS-wide allocator, which is exactly
    // why this one is NOT that.
    tox_heap_block_t* prev = 0;
    for (tox_heap_block_t* p = g_first; p && p != b; p = p->next) prev = p;
    if (prev && prev->free) {
        if (g_last == b) g_last = prev;
        prev->size += BLOCK_HDR_SIZE + b->size;
        prev->next = b->next;
    }
}

void* tox_realloc(void* ptr, uint64_t old_size, uint64_t new_size) {
    if (!ptr) return tox_alloc(new_size);
    if (new_size == 0) { tox_free(ptr); return 0; }

    void* np = tox_alloc(new_size);
    if (!np) return 0; // original block left untouched, matches realloc(3)'s contract

    uint64_t copy = old_size < new_size ? old_size : new_size;
    uint8_t* dst = (uint8_t*)np;
    const uint8_t* src = (const uint8_t*)ptr;
    for (uint64_t i = 0; i < copy; i++) dst[i] = src[i];

    tox_free(ptr);
    return np;
}

void tox_heap_stats(tox_heap_stats_t* out) {
    out->used_bytes = 0;
    out->free_bytes = 0;
    out->block_count = 0;
    for (tox_heap_block_t* b = g_first; b; b = b->next) {
        if (b->free) out->free_bytes += b->size;
        else out->used_bytes += b->size;
        out->block_count++;
    }
}
