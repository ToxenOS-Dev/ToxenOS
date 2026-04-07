#include <stdint.h>
#include "../include/mm.h"

#define HEAP_SIZE       (16 * 1024 * 1024)  // 16MB
#define BLOCK_SIZE      16                   // minimum allocation unit
#define MAGIC_FREE      0xDEADBEEF
#define MAGIC_USED      0xCAFEBABE

// Each block has a header at the front and a footer at the back.
// The footer stores only the block size, which lets kfree() find the
// previous block's header by walking backward — enabling left-side coalescing.
//
// Memory layout per block:
//   [ block_header_t ][ payload bytes... ][ block_footer_t ]
//                      ^-- returned to caller
typedef struct block_header
{
    uint32_t magic;
    uint32_t size;              // payload size (excludes header and footer)
    struct block_header* next;
    struct block_header* prev;  // doubly-linked for O(1) left merge
} block_header_t;

typedef struct {
    uint32_t size;  // mirrors header->size so we can find the header from the end
} block_footer_t;

extern uint32_t kernel_end;

static block_header_t* heap_start = 0;
static uint32_t heap_used = 0;

// Write the footer for a block (call after setting header->size).
static inline void set_footer(block_header_t* h)
{
    block_footer_t* f = (block_footer_t*)((uint8_t*)h
                        + sizeof(block_header_t) + h->size);
    f->size = h->size;
}

void mm_init()
{
    heap_start = (block_header_t*)&kernel_end;
    heap_start->magic = MAGIC_FREE;
    heap_start->size  = HEAP_SIZE - sizeof(block_header_t) - sizeof(block_footer_t);
    heap_start->next  = 0;
    heap_start->prev  = 0;
    set_footer(heap_start);
}

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

void* kmalloc(uint32_t size)
{
    if (size == 0) return 0;

    size = (size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);

    block_header_t* current = heap_start;

    while (current)
    {
        if (current->magic == MAGIC_FREE && current->size >= size)
        {
            uint32_t overhead  = sizeof(block_header_t) + sizeof(block_footer_t);
            uint32_t remaining = current->size - size;

            // Only split if the remainder can hold a header + footer + at least BLOCK_SIZE payload
            if (remaining >= overhead + BLOCK_SIZE)
            {
                // Carve a new free block out of the tail
                block_header_t* new_block =
                    (block_header_t*)((uint8_t*)current + sizeof(block_header_t) + size + sizeof(block_footer_t));

                new_block->magic = MAGIC_FREE;
                new_block->size  = remaining - overhead;
                new_block->next  = current->next;
                new_block->prev  = current;
                set_footer(new_block);

                if (current->next)
                    current->next->prev = new_block;

                current->next = new_block;
                current->size = size;
            }

            current->magic = MAGIC_USED;
            heap_used += current->size;
            set_footer(current);

            return (void*)((uint8_t*)current + sizeof(block_header_t));
        }

        current = current->next;
    }

    return 0;
}

void kfree(void* ptr)
{
    if (!ptr) return;

    block_header_t* header = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    if (header->magic != MAGIC_USED) return;

    header->magic = MAGIC_FREE;
    heap_used -= header->size;

    // ── Right coalesce: merge with next block if it is free ──────────────────
    if (header->next && header->next->magic == MAGIC_FREE)
    {
        block_header_t* next = header->next;
        header->size += sizeof(block_header_t) + sizeof(block_footer_t) + next->size;
        header->next  = next->next;
        if (next->next)
            next->next->prev = header;
        set_footer(header);
    }

    // ── Left coalesce: merge with previous block if it is free ───────────────
    // Find the previous block's header via the footer that sits just before us.
    if ((uint8_t*)header > (uint8_t*)heap_start)
    {
        block_footer_t* prev_footer =
            (block_footer_t*)((uint8_t*)header - sizeof(block_footer_t));
        block_header_t* prev =
            (block_header_t*)((uint8_t*)header
                              - sizeof(block_footer_t)
                              - prev_footer->size
                              - sizeof(block_header_t));

        if (prev->magic == MAGIC_FREE)
        {
            prev->size += sizeof(block_header_t) + sizeof(block_footer_t) + header->size;
            prev->next  = header->next;
            if (header->next)
                header->next->prev = prev;
            set_footer(prev);
            // header is now absorbed — don't touch it further
        }
    }
}

uint32_t mm_used() { return heap_used; }
uint32_t mm_free() { return HEAP_SIZE - heap_used; }
