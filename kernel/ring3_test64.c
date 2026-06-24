// kernel/ring3_test64.c — Milestone 3B: minimal user-accessible paging
// for the hardcoded ring3 smoke test.
//
// Current Milestone-1 paging is entirely supervisor-only: pd[0..31] are
// flat 2MB leaves with no PAGE_USER bit anywhere in the walk. This file
// carves exactly one of those entries (PD_RING3_IDX, deliberately the
// last/highest-indexed one -- furthest from the kernel image, which
// starts at the very first 2MB) into a 4KB page table with two
// user-accessible leaves: one read+execute code page (the hardcoded
// stub) and one read+write stack page. Every other pd[] entry, and
// every other page reachable through the same pml4/pdpt entries, keeps
// its existing supervisor-only (U=0) leaf -- flipping PAGE_USER on the
// pml4/pdpt entries above PD_RING3_IDX only *permits* a user walk to
// continue downward; it does not grant access to sibling leaves, since
// the access check is the AND of the U bit at every level down to the
// actual leaf.
#include <stdint.h>
#include "../include/ring3_test64.h"
#include "../include/memmap64.h"

#define PAGE_PRESENT  0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_USER     0x004ULL

// Fixed tree indices for the canonical higher-half base, matching
// boot64.asm's PML4_HIGH_IDX/PDPT_HIGH_IDX. PD_RING3_IDX is the last of
// the 32 currently-mapped 2MB PD entries (MAP_PD_ENTRIES in boot64.asm)
// -- physical memory far past the small kernel image, safe to repurpose.
#define PML4_HIGH_IDX 511
#define PDPT_HIGH_IDX 510
#define PD_RING3_IDX  31

// pml4/pdpt_high/pd are linked in .bootdata (low/identity VA == PA,
// kept resident per boot64.asm's header comment) -- NOT high-half
// symbols, unlike everything else in this file. Their own address is
// already the physical address; do not subtract KERNEL_VIRT_BASE64
// from them the way phys_of() does below for actual high-half globals.
extern uint64_t pml4[512];
extern uint64_t pdpt_high[512];
extern uint64_t pd[512];

extern void ring3_enter64(uint64_t user_rip, uint64_t user_rsp);

static uint64_t ring3_pt[512] __attribute__((aligned(4096)));
static uint8_t  ring3_code_page[4096] __attribute__((aligned(4096)));
static uint8_t  ring3_stack_page[4096] __attribute__((aligned(4096)));

// Milestone 5: the stub now calls sys64_write/sys64_exit instead of
// deliberately faulting -- real assembly (kernel/ring3_syscall_stub64.asm),
// assembled as a flat binary and embedded via objcopy (same technique
// the 32-bit Makefile already uses for init.elf/shell.nex), not a
// hand-encoded byte array (Milestone 3B's 4-byte ud2;jmp$ was simple
// enough to hand-encode; calling two syscalls with a string argument
// is not).
extern const uint8_t _binary_build_ring3_syscall_stub64_bin_start[];
extern const uint8_t _binary_build_ring3_syscall_stub64_bin_end[];

static inline uint64_t phys_of(const void* high_half_ptr) {
    return (uint64_t)high_half_ptr - KERNEL_VIRT_BASE64;
}

static void ring3_map_init(void) {
    for (uint32_t i = 0; i < sizeof(ring3_code_page); i++) ring3_code_page[i] = 0;
    uint32_t stub_size = (uint32_t)(_binary_build_ring3_syscall_stub64_bin_end -
                                     _binary_build_ring3_syscall_stub64_bin_start);
    for (uint32_t i = 0; i < stub_size && i < sizeof(ring3_code_page); i++)
        ring3_code_page[i] = _binary_build_ring3_syscall_stub64_bin_start[i];

    for (int i = 0; i < 512; i++) ring3_pt[i] = 0;
    ring3_pt[0] = phys_of(ring3_code_page)  | PAGE_PRESENT | PAGE_USER;               // read+exec, not writable
    ring3_pt[1] = phys_of(ring3_stack_page) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    pd[PD_RING3_IDX]        = phys_of(ring3_pt) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    pdpt_high[PDPT_HIGH_IDX] |= PAGE_USER;
    pml4[PML4_HIGH_IDX]      |= PAGE_USER;

    // Reloading CR3 with its current value forces a full TLB flush
    // (architecturally guaranteed outside of PCID no-flush mode, which
    // isn't in use here) -- needed since pd[PD_RING3_IDX] just changed
    // from a 2MB leaf to a page-table pointer.
    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)pml4) : "memory");
}

void ring3_test64_start(void) {
    ring3_map_init();

    uint64_t user_rip      = KERNEL_VIRT_BASE64 + (PD_RING3_IDX * 0x200000ULL) + 0x000;
    uint64_t user_stack_top = KERNEL_VIRT_BASE64 + (PD_RING3_IDX * 0x200000ULL) + 0x1000 + 0x1000;

    ring3_enter64(user_rip, user_stack_top);
    // never reached -- ring3_enter64 ends in iretq
}
