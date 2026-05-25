// ToxenOS/kernel/mm.c — kernel heap allocator
//
// Segregated free list (SFL) with 32 size classes, one per power of two
// from 2^4 (16 bytes) up to 2^35 (but capped at HEAP_SIZE).
//
// Each size class has its own free list.  Allocation finds the smallest
// class that fits, pops a block, and optionally splits the remainder into
// a smaller class.  Free pushes the block back to its class and coalesces
// with its physical neighbour if it is also free.
//
// Physical layout inside the heap (contiguous, no fragmentation):
//
//   [ block_header_t ][ payload ... ][ block_footer_t ]
//
// header.size = payload bytes (does NOT include header + footer).
// footer.size = same value, used for backward merge (O(1) left coalesce).
// header.size_class = which free-list bucket owns this block (for re-insertion).
//
// The heap starts at &kernel_end and is 16MB.  This does NOT go through the
// PMM — the PMM manages physical frames for user-space pages.

#include <stdint.h>
#include "../include/mm.h"

// ── Constants ─────────────────────────────────────────────────────────────────
#define HEAP_SIZE      (16u * 1024u * 1024u)   // 16 MB
#define MIN_BLOCK      16u                      // minimum payload (2^4)
#define NUM_CLASSES    32                        // size classes 0..31
#define MAGIC_FREE     0xDEADBEEFu
#define MAGIC_USED     0xCAFEBABEu

// ── Block metadata ────────────────────────────────────────────────────────────
typedef struct block_hdr {
    uint32_t magic;
    uint32_t size;           // payload bytes
    uint8_t  size_class;     // which free-list this block belongs to
    uint8_t  _pad[3];
    struct block_hdr* next;  // free list link (only valid when free)
    struct block_hdr* prev;  // free list link (only valid when free)
} block_hdr_t;

typedef struct {
    uint32_t size;           // mirrors block_hdr_t.size for backward merge
} block_ftr_t;

#define HDR_SIZE  sizeof(block_hdr_t)
#define FTR_SIZE  sizeof(block_ftr_t)
#define OVERHEAD  (HDR_SIZE + FTR_SIZE)

// ── Free list table ───────────────────────────────────────────────────────────
static block_hdr_t* free_lists[NUM_CLASSES];

// ── Heap bookkeeping ──────────────────────────────────────────────────────────
extern uint32_t kernel_end;
static uint8_t*    heap_base  = 0;
static uint32_t    heap_used  = 0;

// ── Size class helpers ────────────────────────────────────────────────────────
// Returns the index of the smallest size class that can hold `size` bytes.
static inline int size_to_class(uint32_t size)
{
    if (size < MIN_BLOCK) size = MIN_BLOCK;
    int cls = 0;
    uint32_t s = MIN_BLOCK;
    while (s < size && cls < NUM_CLASSES - 1) { s <<= 1; cls++; }
    return cls;
}

// ── Footer access ─────────────────────────────────────────────────────────────
static inline block_ftr_t* hdr_to_ftr(block_hdr_t* h)
{
    return (block_ftr_t*)((uint8_t*)h + HDR_SIZE + h->size);
}

static inline void write_footer(block_hdr_t* h)
{
    hdr_to_ftr(h)->size = h->size;
}

// ── Free list operations ──────────────────────────────────────────────────────
static void fl_push(block_hdr_t* b)
{
    int cls = size_to_class(b->size);
    b->size_class = (uint8_t)cls;
    b->next = free_lists[cls];
    b->prev = 0;
    if (free_lists[cls]) free_lists[cls]->prev = b;
    free_lists[cls] = b;
}

static void fl_remove(block_hdr_t* b)
{
    int cls = b->size_class;
    if (b->prev) b->prev->next = b->next;
    else         free_lists[cls] = b->next;
    if (b->next) b->next->prev = b->prev;
    b->next = b->prev = 0;
}

// ── mm_init ───────────────────────────────────────────────────────────────────
// ramdisk_phys_end: if non-zero, heap starts at max(kernel_end, virt(ramdisk_end))
// so the heap never overlaps the in-memory ramdisk.
void mm_init(uint32_t ramdisk_phys_end)
{
    for (int i = 0; i < NUM_CLASSES; i++) free_lists[i] = 0;

    heap_base = (uint8_t*)&kernel_end;
    if (ramdisk_phys_end) {
        uint8_t* rd_virt_end = (uint8_t*)(ramdisk_phys_end + 0xC0000000u);
        if (rd_virt_end > heap_base) heap_base = rd_virt_end;
    }
    heap_used = 0;

    // One giant free block covering the entire heap
    block_hdr_t* b = (block_hdr_t*)heap_base;
    b->magic = MAGIC_FREE;
    b->size  = HEAP_SIZE - OVERHEAD;
    b->next  = 0;
    b->prev  = 0;
    write_footer(b);
    fl_push(b);
}

// ── kmalloc ───────────────────────────────────────────────────────────────────
void* kmalloc(uint32_t size)
{
    if (size == 0) return 0;

    // Round up to MIN_BLOCK alignment
    size = (size + MIN_BLOCK - 1) & ~(MIN_BLOCK - 1u);

    // Find the smallest class with a free block
    int cls = size_to_class(size);
    block_hdr_t* b = 0;

    for (int c = cls; c < NUM_CLASSES; c++) {
        if (free_lists[c] && free_lists[c]->size >= size) {
            b = free_lists[c];
            break;
        }
    }

    if (!b) return 0;   // out of memory
    fl_remove(b);

    // Split if the remainder is large enough to be useful
    uint32_t remainder = b->size - size;
    if (remainder >= OVERHEAD + MIN_BLOCK) {
        // Carve a new free block from the tail
        block_hdr_t* split = (block_hdr_t*)((uint8_t*)b + HDR_SIZE + size + FTR_SIZE);
        split->magic = MAGIC_FREE;
        split->size  = remainder - OVERHEAD;
        split->next  = 0;
        split->prev  = 0;
        write_footer(split);
        fl_push(split);

        b->size = size;
        write_footer(b);
    }

    b->magic = MAGIC_USED;
    heap_used += b->size;
    return (void*)((uint8_t*)b + HDR_SIZE);
}

// ── kfree ─────────────────────────────────────────────────────────────────────
void kfree(void* ptr)
{
    if (!ptr) return;

    block_hdr_t* b = (block_hdr_t*)((uint8_t*)ptr - HDR_SIZE);
    if (b->magic != MAGIC_USED) return;

    b->magic = MAGIC_FREE;
    heap_used -= b->size;

    // ── Right coalesce ───────────────────────────────────────────────────────
    block_hdr_t* right = (block_hdr_t*)((uint8_t*)b + HDR_SIZE + b->size + FTR_SIZE);
    uint8_t* heap_end  = heap_base + HEAP_SIZE;
    if ((uint8_t*)right < heap_end && right->magic == MAGIC_FREE) {
        fl_remove(right);
        b->size += OVERHEAD + right->size;
        write_footer(b);
    }

    // ── Left coalesce ────────────────────────────────────────────────────────
    if ((uint8_t*)b > heap_base) {
        block_ftr_t* left_ftr = (block_ftr_t*)((uint8_t*)b - FTR_SIZE);
        block_hdr_t* left     = (block_hdr_t*)((uint8_t*)b - FTR_SIZE - left_ftr->size - HDR_SIZE);
        if (left->magic == MAGIC_FREE) {
            fl_remove(left);
            left->size += OVERHEAD + b->size;
            write_footer(left);
            b = left;
        }
    }

    fl_push(b);
}

// ── kmalloc_aligned ───────────────────────────────────────────────────────────
// Stores the original pointer just before the aligned address so kfree_aligned
// can recover it.  Layout: [kmalloc hdr][...pad...][orig_ptr][aligned payload]
void* kmalloc_aligned(uint32_t size, uint32_t align)
{
    if (align == 0 || (align & (align - 1)) != 0) return 0;
    uint8_t* raw = (uint8_t*)kmalloc(size + align + sizeof(void*));
    if (!raw) return 0;

    uint32_t addr = (uint32_t)raw + sizeof(void*);
    if (addr % align != 0)
        addr = (addr + align - 1) & ~(align - 1);

    void** orig_slot = (void**)(addr - sizeof(void*));
    *orig_slot = raw;
    return (void*)addr;
}

void kfree_aligned(void* ptr)
{
    if (!ptr) return;
    void** orig_slot = (void**)((uint8_t*)ptr - sizeof(void*));
    kfree(*orig_slot);
}

// ── Diagnostics ───────────────────────────────────────────────────────────────
uint32_t mm_used() { return heap_used; }
uint32_t mm_free() { return HEAP_SIZE - heap_used; }
