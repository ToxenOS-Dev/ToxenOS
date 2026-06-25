#ifndef PAGING64_H
#define PAGING64_H

#include <stdint.h>

// Milestone 8: a minimal per-process x86_64 address space. Each user
// process gets its own pml4/pdpt/pd/pt (4 pages, from physmem64) instead
// of mutating the single shared boot-time pml4/pdpt_high/pd that
// Milestones 6/7 used. The kernel itself (identity map + high-half
// code/data) is copied by VALUE into every process's own table, so it
// stays identically present and identically supervisor-only everywhere;
// only the PD_EXEC_IDX slot (see include/nex64.h) is exclusive to the
// process that owns it.
typedef struct {
    uint64_t pml4_phys;  // 0 = unallocated
    uint64_t pdpt_phys;
    uint64_t pd_phys;
    uint64_t pt_phys;
} paging64_as_t;

// Allocates and populates a fresh address space: copies the boot kernel's
// identity map (pml4[0]) and flat kernel mappings (pd[0..29]) by value,
// and gives the process its own, empty, exclusive PD_EXEC_IDX page table.
// Returns 0 on success, -1 if the pool is exhausted (any pages already
// allocated are freed before returning).
int paging64_create_as(paging64_as_t* as);

// Maps a single 4KB page at `vaddr` (must fall within the PD_EXEC_IDX
// user region) into `as`, allocating a fresh zeroed physical page on
// first use. If the page is already mapped (e.g. two segments sharing a
// page), widens it to writable if requested and returns the EXISTING
// physical address instead of allocating a new one. Returns 0 only on a
// genuine failure (vaddr out of range, or the pool is exhausted).
uint64_t paging64_map_user_page(paging64_as_t* as, uint64_t vaddr, int writable);

// Loads `pml4_phys` into CR3 -- a full TLB flush on this kernel (PCID is
// never enabled), so this is also the sole flush mechanism this design
// needs: switching away from a process's address space invalidates every
// TLB entry for pages about to be freed before paging64_destroy_as runs.
void paging64_switch_to(uint64_t pml4_phys);

// Frees every mapped page in `as` (segment pages and the stack page alike
// -- a single walk over pt_phys's 512 entries, no special-casing) plus
// the pt/pd/pdpt/pml4 pages themselves. Safe to call on a partially-built
// `as` (any zero field is simply skipped). Must only be called once CR3
// no longer points at this address space.
void paging64_destroy_as(paging64_as_t* as);

// Milestone 9: validates that every 4KB page covering [vaddr, vaddr+len)
// already has PAGE_PRESENT set (and PAGE_WRITABLE, if need_write) in
// `as`'s page table -- the opposite of paging64_map_user_page's "map on
// first touch": this NEVER allocates. A syscall handed a garbage user
// pointer must fail here, not silently get a fresh page mapped for it.
// Returns 0 if the whole range is already validly mapped, -1 otherwise
// (out of the user region, length overflow, or any touched page missing
// the required PRESENT/WRITABLE bits).
int paging64_check_user_range(const paging64_as_t* as, uint64_t vaddr,
                               uint64_t len, int need_write);

// Reads CR3. Needed so a nested userproc64_run (Milestone 9's sys_spawn)
// can restore exactly whatever address space was active before IT was
// called, rather than unconditionally the boot pml4.
uint64_t paging64_current_cr3(void);

#endif // PAGING64_H
