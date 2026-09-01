#ifndef USERVM64_H
#define USERVM64_H

#include <stdint.h>
#include "memmap64.h"
#include "paging64.h"

// Milestone 25: userspace virtual-address layout + brk/mmap.
//
// ── Layout ───────────────────────────────────────────────────────────
// Every process's private region has been generalized from a single
// fixed 2MB window (Milestones 6-8) into a full 960MB span:
// pd[32..511] within the process's own per-process PD
// (kernel/paging64.c's `pd_phys`, itself reached via
// pdpt_high[PDPT_HIGH_IDX]). pd[0..29] stays the flat kernel mapping,
// and pd[30]/pd[31] stay unused/zero (pd[31] is kernel/ring3_test64.c's
// own legacy carve-out in the SHARED boot tables -- explicitly never
// reused here so a real process's table can never alias it).
//
//   USER_REGION  [USER_REGION_BASE, USER_REGION_BASE + 960MB)
//     IMAGE      [+0,          +32MB )   -- ELF64/NEX64 PT_LOAD segments
//     HEAP       [+32MB,       +160MB)   -- brk, budget 128MB
//     (96MB gap)
//     MMAP       [+256MB,      +512MB)   -- anonymous mmap, budget 256MB
//     (432MB gap)
//     STACK      [+944MB,      +960MB)   -- grows down from the top;
//                                           only the top 4KB page is
//                                           ever actually mapped this
//                                           milestone (no demand paging)
//
// The gaps are deliberate slack: none of these budgets are remotely
// exhausted by anything this OS runs today, and growing one region's
// budget later won't collide with its neighbors.
#define USER_REGION_PD_START 32
#define USER_REGION_PD_COUNT 480
#define USER_REGION_BASE     (KERNEL_VIRT_BASE64 + (uint64_t)USER_REGION_PD_START * 0x200000ULL)
#define USER_REGION_SIZE     ((uint64_t)USER_REGION_PD_COUNT * 0x200000ULL)
#define USER_REGION_END      (USER_REGION_BASE + USER_REGION_SIZE)

#define USER_IMAGE_BASE      (USER_REGION_BASE)
#define USER_IMAGE_MAX_SIZE  0x02000000ULL   // 32 MB
#define USER_IMAGE_END       (USER_IMAGE_BASE + USER_IMAGE_MAX_SIZE)

#define USER_HEAP_BASE        (USER_REGION_BASE + 0x02000000ULL)
#define USER_HEAP_MAX_SIZE    0x08000000ULL   // 128 MB
#define USER_HEAP_MAX_END     (USER_HEAP_BASE + USER_HEAP_MAX_SIZE)

#define USER_MMAP_BASE        (USER_REGION_BASE + 0x10000000ULL)
#define USER_MMAP_MAX_SIZE    0x10000000ULL   // 256 MB
#define USER_MMAP_MAX_END     (USER_MMAP_BASE + USER_MMAP_MAX_SIZE)

#define USER_STACK_MAX_SIZE   0x01000000ULL   // 16 MB budget (1 page actually mapped)
#define USER_STACK_TOP        (USER_REGION_END)
#define USER_STACK_BASE       (USER_STACK_TOP - USER_STACK_MAX_SIZE)

// ── Per-process mmap bookkeeping ─────────────────────────────────────
// A sorted (by base, non-overlapping), kmalloc'd singly-linked list --
// deliberately not a fixed-size table. Anonymous, private, RW-only
// mappings (no flags field yet -- see kernel/uservm64.c's header
// comment for what's deliberately out of scope this milestone).
typedef struct uservm64_region_s {
    uint64_t base;
    uint64_t size;
    struct uservm64_region_s* next;
} uservm64_region_t;

typedef struct {
    uint64_t heap_start;          // == USER_HEAP_BASE, fixed for the process's lifetime
    uint64_t heap_end;            // current break -- byte granular, userspace-visible value
    uservm64_region_t* mmap_list; // sorted by base; NULL when empty
} uservm64_state_t;

// Resets `vm` for a freshly loaded process -- no heap pages mapped yet,
// no mmap regions.
void uservm64_init(uservm64_state_t* vm);

// brk(2)-style: new_brk == 0 means "query", returning the current break
// in *actual_out without changing anything. Otherwise attempts to grow
// or shrink the break to exactly new_brk (byte granular), mapping/
// unmapping whole pages as needed. On growth failure (out of physical
// memory, or new_brk would exceed USER_HEAP_MAX_END), returns -1 and
// leaves the heap exactly as it was -- any pages mapped during the
// failed attempt are unmapped and freed before returning. Newly mapped
// pages are always zeroed (physmem64_alloc_page's own guarantee).
int uservm64_brk(uservm64_state_t* vm, paging64_as_t* as, uint64_t new_brk, uint64_t* actual_out);

// Allocates a page-rounded anonymous, private, read-write region of at
// least `size` bytes, choosing the address itself (first-fit over the
// current mmap_list, so a freed region's address range can be reused by
// a later mmap of matching-or-smaller size). Returns 0 with *addr_out
// set on success. On any failure partway through mapping the requested
// pages, everything mapped so far for THIS call is unmapped/freed
// before returning -1 -- no partial mapping is ever left behind.
int uservm64_mmap(uservm64_state_t* vm, paging64_as_t* as, uint64_t size, uint64_t* addr_out);

// Unmaps exactly one previously-mmap'd region: `addr`/`size` (rounded up
// to a page count) must match a tracked region exactly -- no partial
// unmap of a region in this milestone. Frees every physical page in it
// and removes it from the tracking list. Returns 0 or -1 (no matching
// region).
int uservm64_munmap(uservm64_state_t* vm, paging64_as_t* as, uint64_t addr, uint64_t size);

// Frees the kmalloc'd mmap-region bookkeeping list (NOT the physical
// pages themselves -- kernel/paging64.c's paging64_destroy_as already
// walks and frees every physical page mapped anywhere in the process's
// user region, heap and mmap pages included, since it doesn't
// distinguish by purpose). Called once from kernel/process64.c as part
// of process exit, alongside paging64_destroy_as.
void uservm64_teardown(uservm64_state_t* vm);

// Runs the Milestone 25 self-test suite (brk growth/shrink/zero-fill/
// bounds, mmap/munmap, multi-page operations, partial-failure rollback,
// cross-page copy validation, isolation between two concurrent
// processes, teardown/reclamation). Logs each case and a final tally
// via klog(). Returns 1 if every case passed. Must be run from the
// kernel/idle context, same as kernel/process64.c's own self-test.
int uservm64_selftest(void);

#endif // USERVM64_H
