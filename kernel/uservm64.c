// kernel/uservm64.c — Milestone 25: brk/mmap on top of the generalized
// paging64 API.
//
// Deliberately out of scope this milestone (see the Milestone 25 plan):
// file-backed mmap, shared mappings, MAP_FIXED/requested addresses,
// per-mapping protection flags other than the implicit "anonymous,
// private, read-write", copy-on-write, demand paging, and any
// unmap-part-of-a-region support -- munmap must name a previously
// returned (addr, size) pair exactly. A small, clearly-bounded ToxenOS
// ABI beats a half-implemented POSIX one.
//
// mmap address selection is a first-fit scan over `mmap_list`, kept
// sorted by base address: this is what makes freed ranges reusable by a
// later mmap of matching-or-smaller size (a gap opens up exactly where
// the freed region used to be, and first-fit finds the lowest gap
// first) without needing a separate free-list structure.
#include <stdint.h>
#include "../include/uservm64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/process64.h"
#include "../include/shm64.h"
#include "../include/memobj64.h"
#include "../include/klog.h"

void uservm64_init(uservm64_state_t* vm) {
    vm->heap_start = USER_HEAP_BASE;
    vm->heap_end   = USER_HEAP_BASE;
    vm->mmap_list  = 0;
}

int uservm64_brk(uservm64_state_t* vm, paging64_as_t* as, uint64_t new_brk, uint64_t* actual_out) {
    if (new_brk == 0) {
        *actual_out = vm->heap_end;
        return 0;
    }
    if (new_brk < USER_HEAP_BASE || new_brk > USER_HEAP_MAX_END) return -1;

    uint64_t old_pages = (vm->heap_end - USER_HEAP_BASE + 0xFFFULL) / 0x1000ULL;
    uint64_t new_pages = (new_brk    - USER_HEAP_BASE + 0xFFFULL) / 0x1000ULL;

    if (new_pages > old_pages) {
        uint64_t i;
        for (i = old_pages; i < new_pages; i++) {
            uint64_t va = USER_HEAP_BASE + i * 0x1000ULL;
            if (paging64_map_new(as, va, PAGING64_WRITE) < 0) break;
        }
        if (i < new_pages) {
            // Roll back exactly what THIS call mapped -- the heap is
            // left exactly as it was before the attempt, never partially
            // extended.
            for (uint64_t j = old_pages; j < i; j++) {
                paging64_unmap_and_free(as, USER_HEAP_BASE + j * 0x1000ULL);
            }
            return -1;
        }
    } else if (new_pages < old_pages) {
        for (uint64_t i = new_pages; i < old_pages; i++) {
            paging64_unmap_and_free(as, USER_HEAP_BASE + i * 0x1000ULL);
        }
    }

    vm->heap_end = new_brk;
    *actual_out = new_brk;
    return 0;
}

// Finds the first gap of at least `size` bytes in [USER_MMAP_BASE,
// USER_MMAP_MAX_END), scanning the sorted region list. Returns 0 if
// none exists.
static uint64_t find_mmap_gap(uservm64_state_t* vm, uint64_t size) {
    uint64_t prev_end = USER_MMAP_BASE;
    for (uservm64_region_t* r = vm->mmap_list; r; r = r->next) {
        if (r->base - prev_end >= size) return prev_end;
        prev_end = r->base + r->size;
    }
    if (USER_MMAP_MAX_END - prev_end >= size) return prev_end;
    return 0;
}

static void insert_sorted(uservm64_state_t* vm, uservm64_region_t* node) {
    uservm64_region_t** pp = &vm->mmap_list;
    while (*pp && (*pp)->base < node->base) pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;
}

int uservm64_mmap(uservm64_state_t* vm, paging64_as_t* as, uint64_t size, uint64_t* addr_out) {
    if (size == 0) return -1;
    uint64_t pages = (size + 0xFFFULL) / 0x1000ULL;
    uint64_t rounded = pages * 0x1000ULL;

    uint64_t base = find_mmap_gap(vm, rounded);
    if (!base) return -1;

    uservm64_region_t* node = (uservm64_region_t*)kmalloc(sizeof(uservm64_region_t));
    if (!node) return -1;

    uint64_t i;
    for (i = 0; i < pages; i++) {
        if (paging64_map_new(as, base + i * 0x1000ULL, PAGING64_WRITE) < 0) break;
    }
    if (i < pages) {
        for (uint64_t j = 0; j < i; j++) paging64_unmap_and_free(as, base + j * 0x1000ULL);
        kfree(node);
        return -1;
    }

    node->base = base;
    node->size = rounded;
    node->kind = UVM64_REGION_ANON;
    node->shm  = 0;
    node->next = 0;
    insert_sorted(vm, node);

    *addr_out = base;
    return 0;
}

// M+1A: bump this (and nothing else in this function's structure) the
// day memobj64 gains a real scatter-gather backing kind -- see
// include/memobj64.h's own header comment. Every memobj64_t today has
// exactly one contiguous run, so 1 is always sufficient; this constant
// exists so that fact is asserted once, here, rather than assumed
// silently by a flat base_phys+i*4096 loop the way this function used
// to read shm->base_phys/shm->npages directly.
#define UVM64_MAP_SHM_MAX_RUNS 1

int uservm64_map_shm(uservm64_state_t* vm, paging64_as_t* as, shm64_t* shm, int writable, uint64_t* addr_out) {
    memobj64_run_t runs[UVM64_MAP_SHM_MAX_RUNS];
    int nruns = memobj64_get_runs(shm->obj, runs, UVM64_MAP_SHM_MAX_RUNS);
    if (nruns < 1) return -1; // no backing, or more runs than this caller is sized for

    uint64_t total_pages = 0;
    for (int r = 0; r < nruns; r++) total_pages += runs[r].npages;
    uint64_t size = total_pages * 0x1000ULL;

    uint64_t base = find_mmap_gap(vm, size);
    if (!base) return -1;

    uservm64_region_t* node = (uservm64_region_t*)kmalloc(sizeof(uservm64_region_t));
    if (!node) return -1;

    uint32_t flags = writable ? PAGING64_WRITE : 0;
    uint64_t mapped = 0; // pages successfully mapped so far, across every run -- what rollback undoes
    int ok = 1;
    for (int r = 0; ok && r < nruns; r++) {
        for (uint32_t i = 0; i < runs[r].npages; i++) {
            uint64_t phys = runs[r].phys + (uint64_t)i * 0x1000ULL;
            if (paging64_map(as, base + mapped * 0x1000ULL, phys, flags) < 0) { ok = 0; break; }
            mapped++;
        }
    }
    if (!ok) {
        // Roll back exactly what THIS call mapped -- these PTEs are
        // cleared only (paging64_unmap, never _and_free): the physical
        // pages belong to `shm`, not to this mapping attempt.
        for (uint64_t j = 0; j < mapped; j++) paging64_unmap(as, base + j * 0x1000ULL, 0);
        kfree(node);
        return -1;
    }

    // Success is certain now -- this mapping becomes its own reference,
    // independent of whatever handle the caller used to reach `shm`.
    shm64_add_ref(shm);

    node->base = base;
    node->size = size;
    node->kind = UVM64_REGION_SHM;
    node->shm  = shm;
    node->next = 0;
    insert_sorted(vm, node);

    *addr_out = base;
    return 0;
}

int uservm64_munmap(uservm64_state_t* vm, paging64_as_t* as, uint64_t addr, uint64_t size) {
    if (addr & 0xFFFULL) return -1;
    if (size == 0) return -1;
    uint64_t rounded = ((size + 0xFFFULL) / 0x1000ULL) * 0x1000ULL;

    uservm64_region_t** pp = &vm->mmap_list;
    while (*pp && !((*pp)->base == addr && (*pp)->size == rounded)) pp = &(*pp)->next;
    if (!*pp) return -1;

    uservm64_region_t* node = *pp;
    if (node->kind == UVM64_REGION_SHM) {
        for (uint64_t i = 0; i < node->size / 0x1000ULL; i++) {
            paging64_unmap(as, node->base + i * 0x1000ULL, 0); // no phys free -- shm64_t owns the pages
        }
        shm64_release(node->shm); // drop THIS mapping's reference
    } else {
        for (uint64_t i = 0; i < node->size / 0x1000ULL; i++) {
            paging64_unmap_and_free(as, node->base + i * 0x1000ULL);
        }
    }
    *pp = node->next;
    kfree(node);
    return 0;
}

void uservm64_teardown(uservm64_state_t* vm, paging64_as_t* as) {
    uservm64_region_t* r = vm->mmap_list;
    while (r) {
        uservm64_region_t* next = r->next;
        if (r->kind == UVM64_REGION_SHM) {
            for (uint64_t i = 0; i < r->size / 0x1000ULL; i++) {
                paging64_unmap(as, r->base + i * 0x1000ULL, 0);
            }
            shm64_release(r->shm);
        }
        // UVM64_REGION_ANON: nothing to do here -- paging64_destroy_as's
        // blanket "free every still-present page" sweep (called right
        // after this, by kernel/process64.c) reclaims those.
        kfree(r);
        r = next;
    }
    vm->mmap_list = 0;
}

// ── Self-test suite ──────────────────────────────────────────────────
// Most cases below build a standalone paging64_as_t + uservm64_state_t
// pair directly (paging64_create_as/destroy_as), entirely independent
// of kernel/process64.c's scheduler -- this address space is never
// activated as CR3, so paging64_destroy_as's "CR3 must not point at
// this AS" contract is trivially satisfied, and every low-level
// map/unmap/brk/mmap primitive can be exercised with full precision
// (exact zero-fill checks, exact physmem64_stats deltas) without the
// complexity of actually running ring3 code. Only the cases that
// genuinely need real ring3 execution (exit reclaiming everything,
// isolation between two concurrent processes, repeated cycles through
// the real syscall path) spawn user64/brk_mmap_test64.nex64 for real.
#define VMTEST_PROGRAM "/brk_mmap_test64.nex64"

static int setup_as(paging64_as_t* as, uservm64_state_t* vm) {
    if (paging64_create_as(as) < 0) return 0;
    uservm64_init(vm);
    return 1;
}

static void teardown_as(paging64_as_t* as, uservm64_state_t* vm) {
    uservm64_teardown(vm, as);
    paging64_destroy_as(as);
}

static int page_is_all(paging64_as_t* as, uint64_t va, uint8_t val) {
    uint64_t phys = paging64_translate(as, va);
    if (!phys) return 0;
    uint8_t* p = (uint8_t*)physmem64_to_virt(phys);
    for (int i = 0; i < 4096; i++) if (p[i] != val) return 0;
    return 1;
}

static int test_brk_growth_and_zerofill(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;

    uint64_t actual;
    if (uservm64_brk(&vm, &as, 0, &actual) < 0 || actual != USER_HEAP_BASE) ok = 0;

    if (ok && (uservm64_brk(&vm, &as, USER_HEAP_BASE + 0x1000, &actual) < 0 ||
               actual != USER_HEAP_BASE + 0x1000)) ok = 0;
    if (ok && !paging64_is_mapped(&as, USER_HEAP_BASE)) ok = 0;
    if (ok && !page_is_all(&as, USER_HEAP_BASE, 0)) ok = 0;

    // Write a pattern, then grow by 3 more pages -- the old page's data
    // must survive, and the 3 new pages must each be freshly zeroed.
    if (ok) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(paging64_translate(&as, USER_HEAP_BASE));
        for (int i = 0; i < 4096; i++) p[i] = (uint8_t)i;
    }
    if (ok && (uservm64_brk(&vm, &as, USER_HEAP_BASE + 4 * 0x1000, &actual) < 0 ||
               actual != USER_HEAP_BASE + 4 * 0x1000)) ok = 0;
    if (ok) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(paging64_translate(&as, USER_HEAP_BASE));
        for (int i = 0; i < 4096 && ok; i++) if (p[i] != (uint8_t)i) ok = 0;
    }
    for (int pg = 1; pg < 4 && ok; pg++) {
        if (!page_is_all(&as, USER_HEAP_BASE + (uint64_t)pg * 0x1000, 0)) ok = 0;
    }

    teardown_as(&as, &vm);
    return ok;
}

static int test_brk_shrink_and_reclaim(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;
    uint64_t actual;

    if (uservm64_brk(&vm, &as, USER_HEAP_BASE + 4 * 0x1000, &actual) < 0) ok = 0;

    physmem64_stats_t before, after;
    physmem64_stats(&before);

    if (ok && (uservm64_brk(&vm, &as, USER_HEAP_BASE + 0x1000, &actual) < 0 ||
               actual != USER_HEAP_BASE + 0x1000)) ok = 0;

    physmem64_stats(&after);
    if (ok && before.used_pages - after.used_pages != 3) ok = 0; // exactly 3 pages freed
    if (ok && (paging64_is_mapped(&as, USER_HEAP_BASE + 0x1000) ||
               paging64_is_mapped(&as, USER_HEAP_BASE + 2 * 0x1000) ||
               paging64_is_mapped(&as, USER_HEAP_BASE + 3 * 0x1000))) ok = 0;
    if (ok && !paging64_is_mapped(&as, USER_HEAP_BASE)) ok = 0; // remaining page untouched

    teardown_as(&as, &vm);
    return ok;
}

static int test_brk_bounds_rejection(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;

    physmem64_stats_t before, after;
    physmem64_stats(&before);

    uint64_t actual;
    if (uservm64_brk(&vm, &as, USER_HEAP_MAX_END + 0x1000, &actual) == 0) ok = 0; // must fail
    if (vm.heap_end != USER_HEAP_BASE) ok = 0; // heap must be untouched

    physmem64_stats(&after);
    if (before.used_pages != after.used_pages) ok = 0; // nothing leaked

    teardown_as(&as, &vm);
    return ok;
}

static int test_mmap_regions(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;

    uint64_t a1, a2;
    if (uservm64_mmap(&vm, &as, 0x1000, &a1) < 0) ok = 0;
    if (ok && uservm64_mmap(&vm, &as, 3 * 0x1000, &a2) < 0) ok = 0;
    if (ok && !(a2 >= a1 + 0x1000 || a1 >= a2 + 3 * 0x1000)) ok = 0; // non-overlapping

    if (ok) {
        uint8_t* p1 = (uint8_t*)physmem64_to_virt(paging64_translate(&as, a1));
        uint8_t* p2 = (uint8_t*)physmem64_to_virt(paging64_translate(&as, a2));
        p1[0] = 0xAB;
        p2[0] = 0xCD;
        if (p1[0] != 0xAB || p2[0] != 0xCD) ok = 0; // independently writable
    }
    for (uint64_t i = 0; i < 3 && ok; i++) {
        if (!page_is_all(&as, a2 + i * 0x1000, 0) && i != 0) ok = 0; // untouched pages still zero
    }

    teardown_as(&as, &vm);
    return ok;
}

static int test_munmap_and_reuse(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;

    uint64_t a, b, c;
    if (uservm64_mmap(&vm, &as, 0x1000, &a) < 0) ok = 0;
    if (ok && uservm64_mmap(&vm, &as, 0x1000, &b) < 0) ok = 0;
    if (ok && b != a + 0x1000) ok = 0; // bump-allocated back to back

    physmem64_stats_t before, after;
    physmem64_stats(&before);

    if (ok && uservm64_munmap(&vm, &as, a, 0x1000) < 0) ok = 0;
    if (ok && uservm64_mmap(&vm, &as, 0x1000, &c) < 0) ok = 0;
    if (ok && c != a) ok = 0; // first-fit finds the freed gap first

    physmem64_stats(&after);
    if (ok && before.used_pages != after.used_pages) ok = 0; // net zero: freed 1, allocated 1
    if (ok && !paging64_is_mapped(&as, b)) ok = 0; // untouched region survives

    teardown_as(&as, &vm);
    return ok;
}

static int test_cross_page_and_invalid_ranges(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;
    uint64_t actual;

    if (uservm64_brk(&vm, &as, USER_HEAP_BASE + 2 * 0x1000, &actual) < 0) ok = 0;

    // A range straddling two independently-backed pages must validate
    // as one contiguous virtual range even though the physical pages
    // behind it are two unrelated physmem64 allocations.
    uint64_t straddle = USER_HEAP_BASE + 0x1000 - 100;
    if (ok && paging64_check_user_range(&as, straddle, 200, 1) != 0) ok = 0;

    // First page valid, second page (past heap_end) unmapped -- the
    // WHOLE range must be rejected, not just the unmapped tail.
    uint64_t spill = USER_HEAP_BASE + 2 * 0x1000 - 100;
    if (ok && paging64_check_user_range(&as, spill, 200, 0) == 0) ok = 0;

    // Completely unmapped address (inside the user region, but nothing
    // ever grew there).
    if (ok && paging64_check_user_range(&as, USER_MMAP_BASE, 8, 0) == 0) ok = 0;

    // Kernel-space address -- must never validate regardless of length.
    if (ok && paging64_check_user_range(&as, KERNEL_VIRT_BASE64, 8, 0) == 0) ok = 0;

    // Overflow/wraparound length.
    if (ok && paging64_check_user_range(&as, USER_HEAP_BASE, ~0ULL, 0) == 0) ok = 0;

    teardown_as(&as, &vm);
    return ok;
}

static int test_partial_rollback_on_exhaustion(void) {
    paging64_as_t as; uservm64_state_t vm;
    if (!setup_as(&as, &vm)) return 0;
    int ok = 1;

    physmem64_stats_t stats;
    physmem64_stats(&stats);
    uint64_t drain = stats.free_pages > 3 ? stats.free_pages - 3 : 0;
    uint64_t drained = drain ? physmem64_alloc_pages(drain) : 0;
    if (drain && !drained) { teardown_as(&as, &vm); return 0; } // couldn't set up the test

    physmem64_stats_t before;
    physmem64_stats(&before); // exactly 3 (or whatever's left) free

    // Ask for 5 pages -- only ~3 are available, so this must fail
    // outright and roll back whatever it managed to map first.
    uint64_t actual;
    int grew = uservm64_brk(&vm, &as, USER_HEAP_BASE + 5 * 0x1000, &actual);

    physmem64_stats_t after;
    physmem64_stats(&after);

    if (grew == 0) ok = 0; // must have failed
    if (vm.heap_end != USER_HEAP_BASE) ok = 0; // heap untouched
    if (before.free_pages != after.free_pages) ok = 0; // no net pages consumed

    if (drained) physmem64_free_pages(drained, drain);
    teardown_as(&as, &vm);
    return ok;
}

static int test_process_exit_reclaims(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    uint32_t pid = 0;
    if (process64_spawn(VMTEST_PROGRAM, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;
    int code = process64_wait(pid);

    physmem64_stats(&after);
    return code == 42 && before.used_pages == after.used_pages && before.free_pages == after.free_pages;
}

static int test_isolation_two_processes(void) {
    uint32_t pid_a = 0, pid_b = 0;
    if (process64_spawn(VMTEST_PROGRAM, "isolation", 0, &pid_a) < 0) return 0;
    if (process64_spawn(VMTEST_PROGRAM, "isolation", 0, &pid_b) < 0) return 0;
    if (pid_a == 0 || pid_b == 0 || pid_a == pid_b) return 0;

    int code_a = process64_wait(pid_a);
    int code_b = process64_wait(pid_b);
    return code_a == 42 && code_b == 42;
}

static int test_repeated_cycles_no_leak(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    for (int i = 0; i < 15; i++) {
        uint32_t pid = 0;
        if (process64_spawn(VMTEST_PROGRAM, "", 0, &pid) < 0) return 0;
        if (pid == 0) return 0;
        if (process64_wait(pid) != 42) return 0;
    }

    physmem64_stats(&after);
    return before.used_pages == after.used_pages && before.free_pages == after.free_pages;
}

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

#define USERVM64_TEST(name, expr) do {          \
    int _r = (expr);                            \
    klog("uservm64_selftest: " name " ");       \
    klog(_r ? "PASS\n" : "FAIL\n");             \
    if (_r) pass++; else fail++;                \
} while (0)

int uservm64_selftest(void) {
    int pass = 0, fail = 0;
    klog("uservm64_selftest: starting\n");

    USERVM64_TEST("brk growth + zero-fill", test_brk_growth_and_zerofill());
    USERVM64_TEST("brk shrink + physical reclaim", test_brk_shrink_and_reclaim());
    USERVM64_TEST("brk bounds rejection", test_brk_bounds_rejection());
    USERVM64_TEST("mmap multiple regions", test_mmap_regions());
    USERVM64_TEST("munmap + address reuse", test_munmap_and_reuse());
    USERVM64_TEST("cross-page + invalid range validation", test_cross_page_and_invalid_ranges());
    USERVM64_TEST("partial multi-page rollback on exhaustion", test_partial_rollback_on_exhaustion());
    USERVM64_TEST("process exit reclaims heap+mmap", test_process_exit_reclaims());
    USERVM64_TEST("isolation between two concurrent processes", test_isolation_two_processes());
    USERVM64_TEST("repeated alloc/free cycles, no page leak", test_repeated_cycles_no_leak());

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("uservm64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
