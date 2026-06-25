// kernel/paging64.c — Milestone 8: per-process address-space management.
// Builds on kernel/physmem64.c's fixed-pool allocator and reuses the
// exact carved-PD-entry technique kernel/ring3_test64.c and (pre-M8)
// kernel/exec64.c proved, just per-process instead of globally shared.
#include <stdint.h>
#include "../include/paging64.h"
#include "../include/physmem64.h"
#include "../include/memmap64.h"
#include "../include/nex64.h"
#include "../include/klog.h"

#define PAGE_PRESENT  0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_USER     0x004ULL

#define PML4_HIGH_IDX 511
#define PDPT_HIGH_IDX 510
// PD_EXEC_IDX (30) comes from include/nex64.h. PD_RING3_IDX (31) is
// kernel/ring3_test64.c's own carve-out -- not used by real processes,
// explicitly zeroed below so a process's table never aliases it.
#define PD_RING3_IDX 31

// boot64.asm's static, identity-mapped (VA==PA) page tables -- the values
// being copied here, not phys_of()'d (see memmap64.h's warning).
extern uint64_t pml4[512];
extern uint64_t pd[512];

static void free_if_set(uint64_t phys) {
    if (phys) physmem64_free_page(phys);
}

int paging64_create_as(paging64_as_t* as) {
    as->pml4_phys = 0;
    as->pdpt_phys = 0;
    as->pd_phys   = 0;
    as->pt_phys   = 0;

    as->pml4_phys = physmem64_alloc_page();
    if (!as->pml4_phys) goto fail;
    as->pdpt_phys = physmem64_alloc_page();
    if (!as->pdpt_phys) goto fail;
    as->pd_phys = physmem64_alloc_page();
    if (!as->pd_phys) goto fail;
    as->pt_phys = physmem64_alloc_page();
    if (!as->pt_phys) goto fail;

    uint64_t* npml4 = (uint64_t*)phys_to_ptr(as->pml4_phys);
    uint64_t* npdpt = (uint64_t*)phys_to_ptr(as->pdpt_phys);
    uint64_t* npd   = (uint64_t*)phys_to_ptr(as->pd_phys);

    // Verbatim copy -- preserves the boot identity entry's exact flags
    // (no PAGE_USER), sharing the same physical RAM/page tables every
    // process needs for low-address kernel conveniences (VGA, multiboot
    // info) without granting ring3 access to any of it.
    npml4[0] = pml4[0];
    npml4[PML4_HIGH_IDX] = as->pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    npdpt[PDPT_HIGH_IDX] = as->pd_phys   | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    // Verbatim copy of every boot kernel flat 2MB leaf -- same physical
    // kernel code/data, U=0 preserved exactly because the whole 8-byte
    // entry (including the PS bit) is copied, not reconstructed. This is
    // what keeps the kernel itself, IDT, GDT, TSS, and klog's buffer
    // identically reachable and identically supervisor-only under every
    // process's own table.
    for (int i = 0; i < PD_EXEC_IDX; i++) npd[i] = pd[i];
    npd[PD_EXEC_IDX]  = as->pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    npd[PD_RING3_IDX] = 0;

    return 0;

fail:
    klog("paging64: create_as: pool exhausted\n");
    paging64_destroy_as(as);
    return -1;
}

uint64_t paging64_map_user_page(paging64_as_t* as, uint64_t vaddr, int writable) {
    if (vaddr < USER64_ELF_BASE || vaddr >= USER64_ELF_BASE + 0x200000ULL) {
        klog("paging64: map_user_page: vaddr outside the user region\n");
        return 0;
    }

    uint64_t slot = (vaddr - USER64_ELF_BASE) / 0x1000;
    uint64_t* pt = (uint64_t*)phys_to_ptr(as->pt_phys);

    if (pt[slot] & PAGE_PRESENT) {
        if (writable) pt[slot] |= PAGE_WRITABLE;
        return pt[slot] & ~0xFFFULL;
    }

    uint64_t phys = physmem64_alloc_page();
    if (!phys) return 0;

    pt[slot] = phys | PAGE_PRESENT | PAGE_USER | (writable ? PAGE_WRITABLE : 0);
    return phys;
}

void paging64_switch_to(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

int paging64_check_user_range(const paging64_as_t* as, uint64_t vaddr,
                               uint64_t len, int need_write) {
    if (len == 0) return 0;
    // Reject up front rather than letting vaddr+len wrap around uint64_t
    // for a hostile huge length, and cap to one user region's worth --
    // no syscall this milestone ever legitimately needs more.
    if (len > 0x200000ULL || vaddr + len < vaddr) return -1;
    if (vaddr < USER64_ELF_BASE || vaddr + len > USER64_ELF_BASE + 0x200000ULL) return -1;
    if (!as->pt_phys) return -1;

    uint64_t* pt = (uint64_t*)phys_to_ptr(as->pt_phys);
    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + len + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t page_va = start; page_va < end; page_va += 0x1000) {
        uint64_t slot = (page_va - USER64_ELF_BASE) / 0x1000;
        if (slot >= 512) return -1;
        if (!(pt[slot] & PAGE_PRESENT)) return -1;
        if (need_write && !(pt[slot] & PAGE_WRITABLE)) return -1;
    }
    return 0;
}

uint64_t paging64_current_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

void paging64_destroy_as(paging64_as_t* as) {
    if (as->pt_phys) {
        uint64_t* pt = (uint64_t*)phys_to_ptr(as->pt_phys);
        for (int i = 0; i < 512; i++) {
            if (pt[i] & PAGE_PRESENT) physmem64_free_page(pt[i] & ~0xFFFULL);
        }
    }

    free_if_set(as->pt_phys);
    free_if_set(as->pd_phys);
    free_if_set(as->pdpt_phys);
    free_if_set(as->pml4_phys);

    as->pml4_phys = 0;
    as->pdpt_phys = 0;
    as->pd_phys   = 0;
    as->pt_phys   = 0;
}
