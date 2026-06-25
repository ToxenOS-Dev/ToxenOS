// kernel/physmem64.c — Milestone 8: fixed-pool physical frame allocator.
// See include/physmem64.h for scope/rationale (deliberately not a real
// multiboot-mmap-driven physical memory manager).
#include <stdint.h>
#include "../include/physmem64.h"
#include "../include/memmap64.h"
#include "../include/klog.h"

static uint8_t  pool[PHYSMEM64_POOL_PAGES * 4096] __attribute__((aligned(4096)));
// One bit per pool page -- PHYSMEM64_POOL_PAGES is exactly 64, so a single
// uint64_t is the whole bitmap, no word array needed.
static uint64_t used_bitmap = 0;

void physmem64_init(void) {
    used_bitmap = 0;
}

uint64_t physmem64_alloc_page(void) {
    for (int i = 0; i < PHYSMEM64_POOL_PAGES; i++) {
        uint64_t bit = 1ULL << i;
        if (used_bitmap & bit) continue;

        used_bitmap |= bit;
        uint8_t* page = &pool[i * 4096];
        for (int j = 0; j < 4096; j++) page[j] = 0;
        return phys_of(page);
    }
    klog("physmem64: pool exhausted\n");
    return 0;
}

void physmem64_free_page(uint64_t phys) {
    uint64_t pool_phys = phys_of(pool);
    if (phys < pool_phys || phys >= pool_phys + sizeof(pool) || (phys & 0xFFFULL) != 0) {
        klog("physmem64: free_page: address outside pool -- ignoring\n");
        return;
    }

    int i = (int)((phys - pool_phys) / 4096);
    uint64_t bit = 1ULL << i;
    if (!(used_bitmap & bit)) {
        klog("physmem64: double free -- ignoring\n");
        return;
    }
    used_bitmap &= ~bit;
}
