#ifndef PAGING64_H
#define PAGING64_H

#include <stdint.h>

// Milestone 8: a minimal per-process x86_64 address space -- Milestone
// 25 generalizes it from one fixed 2MB carve-out to a real page-mapping
// API. Each process gets its own pml4/pdpt/pd (3 pages, from
// physmem64) instead of mutating the single shared boot-time
// pml4/pdpt_high/pd. The kernel itself (identity map + high-half
// code/data + physmem64's direct-map window) is copied by VALUE into
// every process's own table, so it stays identically present and
// identically supervisor-only everywhere. Per-process page TABLES
// (the 4th level, one per used 2MB slot) are now allocated lazily, on
// first use, instead of one eagerly-allocated `pt_phys` -- see
// include/uservm64.h for the userspace region layout this enables.
typedef struct {
    uint64_t pml4_phys;  // 0 = unallocated
    uint64_t pdpt_phys;
    uint64_t pd_phys;
} paging64_as_t;

// Permission flags for paging64_map/map_new/set_perms. A mapped page
// always implies present + user-accessible + readable; WRITE and EXEC
// are independent bits (NX is set on the PTE whenever EXEC is absent --
// see kernel/paging64.c; this kernel unconditionally enables EFER.NXE
// at boot, same precedent as its unchecked PAE assumption).
#define PAGING64_WRITE 0x1
#define PAGING64_EXEC  0x2

// Allocates and populates a fresh address space: copies the boot
// kernel's identity map (pml4[0]), the physmem64 direct-map window
// (pdpt_high[0..509]), and the flat kernel mappings (pd[0..29]) by
// value. The entire user region (pd[32..511], see include/uservm64.h)
// starts completely unmapped -- callers (kernel/exec64.c, brk, mmap)
// populate it on demand via paging64_map_new(). Returns 0 on success,
// -1 if the pool is exhausted (any pages already allocated are freed
// before returning).
int paging64_create_as(paging64_as_t* as);

// Maps physical page `phys` at `vaddr` (must be page-aligned and fall
// within the user region) with the given PAGING64_* permission flags.
// Allocates a fresh page-table page for vaddr's 2MB slot on first use
// in that slot. Fails (-1, nothing changed) if vaddr is out of range,
// not page-aligned, ALREADY mapped (this is a strict primitive -- an
// existing mapping must be unmapped first; see paging64_set_perms if
// only the permissions need to change), or the physical pool is
// exhausted. Invalidates any stale TLB entry for vaddr.
int paging64_map(paging64_as_t* as, uint64_t vaddr, uint64_t phys, uint32_t flags);

// Allocates a fresh zeroed physical page (physmem64_alloc_page's own
// guarantee) and maps it at `vaddr` -- the common case, combining
// physmem64_alloc_page + paging64_map. On failure, frees the page if it
// was allocated; nothing is left mapped or allocated.
int paging64_map_new(paging64_as_t* as, uint64_t vaddr, uint32_t flags);

// Unmaps the page at `vaddr` (must currently be mapped) and, if
// phys_out is non-NULL, returns its physical address WITHOUT freeing
// it -- the caller decides whether to keep or free that page. Always
// invalidates the TLB entry for vaddr. Returns -1 if not mapped.
int paging64_unmap(paging64_as_t* as, uint64_t vaddr, uint64_t* phys_out);

// paging64_unmap() + physmem64_free_page() -- the common case.
int paging64_unmap_and_free(paging64_as_t* as, uint64_t vaddr);

// True if `vaddr` currently has a present mapping.
int paging64_is_mapped(const paging64_as_t* as, uint64_t vaddr);

// Returns the full physical address corresponding to `vaddr` (including
// the in-page byte offset), or 0 if not mapped. Never allocates.
uint64_t paging64_translate(const paging64_as_t* as, uint64_t vaddr);

// Returns the PAGING64_* permission flags currently set on `vaddr`, or
// 0 if not mapped (which is indistinguishable from "mapped with no
// permissions" -- not a real state this kernel ever creates, so this
// ambiguity is harmless in practice).
uint32_t paging64_get_perms(const paging64_as_t* as, uint64_t vaddr);

// Updates the permission flags on an ALREADY mapped page (e.g. widening
// a segment's page from read-only to writable when a later PT_LOAD
// segment shares it -- see kernel/exec64.c). Fails (-1) if not mapped.
// Invalidates the TLB entry for vaddr.
int paging64_set_perms(paging64_as_t* as, uint64_t vaddr, uint32_t flags);

// Loads `pml4_phys` into CR3 -- a full TLB flush on this kernel (PCID is
// never enabled), so this is also the sole flush mechanism a full
// address-space switch needs: switching away from a process's address
// space invalidates every TLB entry for pages about to be freed before
// paging64_destroy_as runs. Live per-page map/unmap/set_perms calls
// (brk, mmap, munmap -- the address space stays active throughout)
// additionally invlpg the specific vaddr touched, since no CR3 switch
// happens to do it for them.
void paging64_switch_to(uint64_t pml4_phys);

// Frees every mapped page anywhere in the user region (pd[32..511] --
// heap, mmap, image, and stack pages alike, no special-casing needed
// since paging64 doesn't track WHY a page was mapped) plus every
// page-table page allocated for those slots, plus the pd/pdpt/pml4
// pages themselves. Safe to call on a partially-built `as` (any zero
// field is simply skipped). Must only be called once CR3 no longer
// points at this address space.
void paging64_destroy_as(paging64_as_t* as);

// Milestone 9: validates that every 4KB page covering [vaddr, vaddr+len)
// falls within the user region and already has PAGE_PRESENT set (and
// PAGE_WRITABLE, if need_write) -- the opposite of paging64_map_new's
// "map on first touch": this NEVER allocates. A syscall handed a
// garbage user pointer must fail here, not silently get a fresh page
// mapped for it. Rejects zero-length-overflow ranges, ranges reaching
// outside the user region (which includes all kernel-space addresses),
// and any range where even one covered page is unmapped or lacks the
// required permission -- including a range whose first page is valid
// and a later page is not. Returns 0 if the whole range is validly
// mapped, -1 otherwise.
int paging64_check_user_range(const paging64_as_t* as, uint64_t vaddr,
                               uint64_t len, int need_write);

// Reads CR3. Needed so a nested process64_spawn+process64_wait can
// restore exactly whatever address space was active before IT was
// called, rather than unconditionally the boot pml4.
uint64_t paging64_current_cr3(void);

#endif // PAGING64_H
