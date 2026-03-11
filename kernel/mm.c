#include <stdint.h>
#include "../include/mm.h"

#define HEAP_SIZE       (16 * 1024 * 1024)  // 16MB
#define BLOCK_SIZE      16                   // minimum allocation unit
#define MAGIC_FREE      0xDEADBEEF
#define MAGIC_USED      0xCAFEBABE

typedef struct block_header
{
    uint32_t magic;
    uint32_t size;
    struct block_header* next;
} block_header_t;

extern uint32_t kernel_end;  // provided by linker

static block_header_t* heap_start = 0;
static uint32_t heap_used = 0;

void mm_init()
{
    heap_start = (block_header_t*)&kernel_end;

    // set up one giant free block covering the whole heap
    heap_start->magic = MAGIC_FREE;
    heap_start->size  = HEAP_SIZE - sizeof(block_header_t);
    heap_start->next  = 0;
}

void* kmalloc(uint32_t size)
{
    if (size == 0) return 0;

    // align size to BLOCK_SIZE
    size = (size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1);

    block_header_t* current = heap_start;

    while (current)
    {
        if (current->magic == MAGIC_FREE && current->size >= size)
        {
            uint32_t remaining = current->size - size - sizeof(block_header_t);

            // split block if enough space left over
            if (remaining > sizeof(block_header_t) + BLOCK_SIZE)
            {
                block_header_t* new_block =
                    (block_header_t*)((uint8_t*)current + sizeof(block_header_t) + size);

                new_block->magic = MAGIC_FREE;
                new_block->size  = remaining;
                new_block->next  = current->next;

                current->next = new_block;
                current->size = size;
            }

            current->magic = MAGIC_USED;
            heap_used += current->size;

            return (void*)((uint8_t*)current + sizeof(block_header_t));
        }

        current = current->next;
    }

    return 0;  // out of memory
}

void kfree(void* ptr)
{
    if (!ptr) return;

    block_header_t* header =
        (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));

    if (header->magic != MAGIC_USED) return;  // double free or corruption

    header->magic = MAGIC_FREE;
    heap_used -= header->size;

    // merge with next block if it's also free (coalescing)
    if (header->next && header->next->magic == MAGIC_FREE)
    {
        header->size += sizeof(block_header_t) + header->next->size;
        header->next  = header->next->next;
    }
}

uint32_t mm_used()
{
    return heap_used;
}

uint32_t mm_free()
{
    return HEAP_SIZE - heap_used;
}