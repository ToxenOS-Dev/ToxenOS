// kernel/paging64.c — Milestone 8: per-process address-space management.
// Milestone 25: generalized from a single fixed 2MB carve-out (one
// eagerly-allocated page table) into a real "map/unmap arbitrary
// page-aligned userspace VA" API, backing the new userspace VA layout
// (include/uservm64.h) and brk/mmap. Page tables for each 2MB PD slot
// (pd[32..511], the user region) are now allocated lazily, on first use
// in that slot, rather than one fixed PT eagerly created for everyone.
#include <stdint.h>
#include "../include/paging64.h"
#include "../include/physmem64.h"
#include "../include/memmap64.h"
#include "../include/uservm64.h"
#include "../include/klog.h"

#define PAGE_PRESENT  0x0001ULL
#define PAGE_WRITABLE 0x0002ULL
#define PAGE_USER     0x0004ULL
#define PAGE_NX       0x8000000000000000ULL
// Physical-address field of a PDE/PTE -- everything except the low
// flag bits and the high NX bit. Every place that extracts a physical
// address out of a raw table entry must use this, not a bare ~0xFFFULL,
// now that entries can have bit 63 (NX) set.
#define PAGE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_HIGH_IDX 511
#define PDPT_HIGH_IDX 510
// PD_RING3_IDX (31) is kernel/ring3_test64.c's own carve-out in the
// SHARED boot tables -- not used by real processes. pd[30] is left
// unused too (a one-slot gap for clear separation from the flat kernel
// mapping below it). Neither needs an explicit zero-write here: a
// freshly allocated pd_phys page is already all-zero
// (physmem64_alloc_page's own guarantee), and the copy loop below only
// ever touches pd[0..29] and the user region pd[32..511].
#define PD_RING3_IDX 31

// boot64.asm's static, identity-mapped (VA==PA) page tables -- the values
// being copied here, not phys_of()'d (see memmap64.h's warning).
extern uint64_t pml4[512];
extern uint64_t pd[512];
// Milestone 23: physmem64.c's physical direct-map window lives in
// pdpt_high[0..509] (slot 510 is the kernel image, handled separately
// below). Every process needs its OWN copy of these entries, exactly
// like the flat kernel `pd[]` entries below -- physmem64_to_virt() is
// called from exec64.c/paging64.c itself while the CALLER's (not yet
// the new process's) CR3 is still active, and for a nested sys_spawn
// that caller is another process, not the boot kernel -- so every
// process's own table must carry a working copy of the direct map or
// that access page-faults. See kernel/physmem64.c's header comment.
extern uint64_t pdpt_high[512];

static inline void invlpg(uint64_t vaddr) {
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

static void free_if_set(uint64_t phys) {
    if (phys) physmem64_free_page(phys);
}

int paging64_create_as(paging64_as_t* as) {
    as->pml4_phys = 0;
    as->pdpt_phys = 0;
    as->pd_phys   = 0;

    as->pml4_phys = physmem64_alloc_page();
    if (!as->pml4_phys) goto fail;
    as->pdpt_phys = physmem64_alloc_page();
    if (!as->pdpt_phys) goto fail;
    as->pd_phys = physmem64_alloc_page();
    if (!as->pd_phys) goto fail;

    uint64_t* npml4 = (uint64_t*)physmem64_to_virt(as->pml4_phys);
    uint64_t* npdpt = (uint64_t*)physmem64_to_virt(as->pdpt_phys);
    uint64_t* npd   = (uint64_t*)physmem64_to_virt(as->pd_phys);

    // Verbatim copy -- preserves the boot identity entry's exact flags
    // (no PAGE_USER), sharing the same physical RAM/page tables every
    // process needs for low-address kernel conveniences (VGA, multiboot
    // info) without granting ring3 access to any of it.
    npml4[0] = pml4[0];
    npml4[PML4_HIGH_IDX] = as->pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    npdpt[PDPT_HIGH_IDX] = as->pd_phys   | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    // Verbatim copy of physmem64's direct-map entries -- same reasoning
    // as the pd[] copy below, just one level up. No PAGE_USER here
    // either: this window is kernel-only in every process, exactly like
    // the flat kernel mapping it sits beside.
    for (int i = 0; i < PDPT_HIGH_IDX; i++) npdpt[i] = pdpt_high[i];

    // Verbatim copy of every boot kernel flat 2MB leaf -- same physical
    // kernel code/data, U=0 preserved exactly because the whole 8-byte
    // entry (including the PS bit) is copied, not reconstructed. This is
    // what keeps the kernel itself, IDT, GDT, TSS, and klog's buffer
    // identically reachable and identically supervisor-only under every
    // process's own table. pd[30..511] (the user region, plus the
    // legacy pd[31] gap) is intentionally left at 0 -- npd was just
    // freshly allocated and zeroed, nothing to do.
    for (int i = 0; i < USER_REGION_PD_START - 2; i++) npd[i] = pd[i];

    return 0;

fail:
    klog("paging64: create_as: pool exhausted\n");
    paging64_destroy_as(as);
    return -1;
}

// Internal: returns a pointer to the PTE for `vaddr` within `as`'s user
// region, or NULL if out of range OR the covering PD slot has no page
// table yet (i.e. vaddr is definitely unmapped). Never allocates --
// callers that want to allocate call ensure_pt() first.
static uint64_t* pte_lookup(const paging64_as_t* as, uint64_t vaddr) {
    if (vaddr < USER_REGION_BASE || vaddr >= USER_REGION_END) return 0;
    if (!as->pd_phys) return 0;

    uint64_t pd_idx = (vaddr - KERNEL_VIRT_BASE64) / 0x200000ULL;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FFULL;

    uint64_t* upd = (uint64_t*)physmem64_to_virt(as->pd_phys);
    if (!(upd[pd_idx] & PAGE_PRESENT)) return 0;

    uint64_t* pt = (uint64_t*)physmem64_to_virt(upd[pd_idx] & PAGE_ADDR_MASK);
    return &pt[pt_idx];
}

// Internal: like pte_lookup, but allocates a fresh page table for
// vaddr's PD slot if one doesn't exist yet. Returns NULL only if vaddr
// is out of range or the physical pool is exhausted.
static uint64_t* pte_lookup_or_create(paging64_as_t* as, uint64_t vaddr) {
    if (vaddr < USER_REGION_BASE || vaddr >= USER_REGION_END) return 0;

    uint64_t pd_idx = (vaddr - KERNEL_VIRT_BASE64) / 0x200000ULL;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FFULL;

    uint64_t* upd = (uint64_t*)physmem64_to_virt(as->pd_phys);
    if (!(upd[pd_idx] & PAGE_PRESENT)) {
        uint64_t pt_phys = physmem64_alloc_page();
        if (!pt_phys) return 0;
        upd[pd_idx] = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }

    uint64_t* pt = (uint64_t*)physmem64_to_virt(upd[pd_idx] & PAGE_ADDR_MASK);
    return &pt[pt_idx];
}

int paging64_map(paging64_as_t* as, uint64_t vaddr, uint64_t phys, uint32_t flags) {
    if (vaddr & 0xFFFULL) return -1;

    uint64_t* pte = pte_lookup_or_create(as, vaddr);
    if (!pte) return -1;
    if (*pte & PAGE_PRESENT) return -1; // strict -- caller must unmap first

    uint64_t entry = phys | PAGE_PRESENT | PAGE_USER;
    if (flags & PAGING64_WRITE) entry |= PAGE_WRITABLE;
    if (!(flags & PAGING64_EXEC)) entry |= PAGE_NX;

    *pte = entry;
    invlpg(vaddr);
    return 0;
}

int paging64_map_new(paging64_as_t* as, uint64_t vaddr, uint32_t flags) {
    uint64_t phys = physmem64_alloc_page();
    if (!phys) return -1;
    if (paging64_map(as, vaddr, phys, flags) < 0) {
        physmem64_free_page(phys);
        return -1;
    }
    return 0;
}

// Internal: once a PTE has just been cleared, checks whether its whole
// page table is now empty and, if so, frees the PT page itself and
// clears its PDE -- otherwise a PT allocated to service a since-rolled-
// back or since-shrunk region stays pinned until the ENTIRE address
// space is torn down, which is a real (if bounded) leak: a failed
// multi-page brk/mmap growth must return physical memory to exactly
// its pre-attempt state, not just unwind the data pages it mapped.
static void free_pt_if_empty(paging64_as_t* as, uint64_t vaddr) {
    uint64_t pd_idx = (vaddr - KERNEL_VIRT_BASE64) / 0x200000ULL;
    uint64_t* upd = (uint64_t*)physmem64_to_virt(as->pd_phys);
    if (!(upd[pd_idx] & PAGE_PRESENT)) return;

    uint64_t pt_phys = upd[pd_idx] & PAGE_ADDR_MASK;
    uint64_t* pt = (uint64_t*)physmem64_to_virt(pt_phys);
    for (int i = 0; i < 512; i++) {
        if (pt[i] & PAGE_PRESENT) return; // still in use
    }

    upd[pd_idx] = 0;
    invlpg(vaddr);
    physmem64_free_page(pt_phys);
}

int paging64_unmap(paging64_as_t* as, uint64_t vaddr, uint64_t* phys_out) {
    uint64_t* pte = pte_lookup(as, vaddr);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;

    uint64_t phys = *pte & PAGE_ADDR_MASK;
    *pte = 0;
    invlpg(vaddr);
    free_pt_if_empty(as, vaddr);
    if (phys_out) *phys_out = phys;
    return 0;
}

int paging64_unmap_and_free(paging64_as_t* as, uint64_t vaddr) {
    uint64_t phys;
    if (paging64_unmap(as, vaddr, &phys) < 0) return -1;
    physmem64_free_page(phys);
    return 0;
}

int paging64_is_mapped(const paging64_as_t* as, uint64_t vaddr) {
    uint64_t* pte = pte_lookup(as, vaddr);
    return pte && (*pte & PAGE_PRESENT);
}

uint64_t paging64_translate(const paging64_as_t* as, uint64_t vaddr) {
    uint64_t* pte = pte_lookup(as, vaddr);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    return (*pte & PAGE_ADDR_MASK) | (vaddr & 0xFFFULL);
}

uint32_t paging64_get_perms(const paging64_as_t* as, uint64_t vaddr) {
    uint64_t* pte = pte_lookup(as, vaddr);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    uint32_t flags = 0;
    if (*pte & PAGE_WRITABLE) flags |= PAGING64_WRITE;
    if (!(*pte & PAGE_NX)) flags |= PAGING64_EXEC;
    return flags;
}

int paging64_set_perms(paging64_as_t* as, uint64_t vaddr, uint32_t flags) {
    uint64_t* pte = pte_lookup(as, vaddr);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;

    uint64_t entry = (*pte & PAGE_ADDR_MASK) | PAGE_PRESENT | PAGE_USER;
    if (flags & PAGING64_WRITE) entry |= PAGE_WRITABLE;
    if (!(flags & PAGING64_EXEC)) entry |= PAGE_NX;

    *pte = entry;
    invlpg(vaddr);
    return 0;
}

void paging64_switch_to(uint64_t pml4_phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

int paging64_check_user_range(const paging64_as_t* as, uint64_t vaddr,
                               uint64_t len, int need_write) {
    if (len == 0) return 0;
    if (vaddr + len < vaddr) return -1; // overflow/wraparound
    if (vaddr < USER_REGION_BASE || vaddr + len > USER_REGION_END) return -1;
    if (!as->pd_phys) return -1;

    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + len + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t page_va = start; page_va < end; page_va += 0x1000ULL) {
        uint64_t* pte = pte_lookup(as, page_va);
        if (!pte || !(*pte & PAGE_PRESENT)) return -1;
        if (need_write && !(*pte & PAGE_WRITABLE)) return -1;
    }
    return 0;
}

uint64_t paging64_current_cr3(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(v));
    return v;
}

void paging64_destroy_as(paging64_as_t* as) {
    if (as->pd_phys) {
        uint64_t* upd = (uint64_t*)physmem64_to_virt(as->pd_phys);
        for (int i = USER_REGION_PD_START; i < 512; i++) {
            if (!(upd[i] & PAGE_PRESENT)) continue;
            uint64_t* pt = (uint64_t*)physmem64_to_virt(upd[i] & PAGE_ADDR_MASK);
            for (int j = 0; j < 512; j++) {
                if (pt[j] & PAGE_PRESENT) physmem64_free_page(pt[j] & PAGE_ADDR_MASK);
            }
            physmem64_free_page(upd[i] & PAGE_ADDR_MASK);
        }
    }

    free_if_set(as->pd_phys);
    free_if_set(as->pdpt_phys);
    free_if_set(as->pml4_phys);

    as->pml4_phys = 0;
    as->pdpt_phys = 0;
    as->pd_phys   = 0;
}
