// kernel/memobj64.c — M+1A: single owner of physical backing + its
// refcount, factored out from underneath kernel/shm64.c. See
// include/memobj64.h for the design rationale and scope (contiguous-
// only today; memobj64_get_runs() is the seam a future scatter-gather
// backing plugs into without changing any existing caller's shape).
#include <stdint.h>
#include "../include/memobj64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/klog.h"

static memobj64_t* g_memobj_list = 0; // intrusive list of every live memobj, diagnostics only

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static inline uint64_t memobj64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void memobj64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

int memobj64_create(uint64_t size, memobj64_t** out) {
    if (size == 0 || !out) return -1;

    // M+1A hardening: shm64_create() used to do this exact rounding
    // completely unguarded (`(size + 0xFFF) / 0x1000` truncated straight
    // into a uint32_t). A `size` near the top of the uint64_t range wraps
    // the addition, and a `size` whose page count exceeds what a uint32_t
    // can hold truncates -- both silently produce a tiny real allocation
    // for a caller that asked for (and believes it got) something
    // enormous. Neither path was reachable with real syscall-sized
    // requests before, but this is now the shared primitive underneath
    // shm64 (and, later, gpu64_buffer), so it's checked explicitly rather
    // than inherited as a latent bug.
    if (size > (0xFFFFFFFFFFFFFFFFULL - 0xFFFULL)) return -1;
    uint64_t rounded = (size + 0xFFFULL) & ~0xFFFULL;
    uint64_t npages64 = rounded / 0x1000ULL;
    if (npages64 > 0xFFFFFFFFULL) return -1;
    uint32_t npages = (uint32_t)npages64;

    uint64_t base_phys = physmem64_alloc_pages(npages);
    if (!base_phys) return -1;

    memobj64_t* m = (memobj64_t*)kmalloc(sizeof(memobj64_t));
    if (!m) { physmem64_free_pages(base_phys, npages); return -1; }

    m->base_phys = base_phys;
    m->npages    = npages;
    m->refcount  = 1;

    uint64_t flags = memobj64_lock();
    m->dbg_next = g_memobj_list;
    g_memobj_list = m;
    memobj64_unlock(flags);

    *out = m;
    return 0;
}

static void unlink_and_free(memobj64_t* m) {
    uint64_t flags = memobj64_lock();
    memobj64_t** pp = &g_memobj_list;
    while (*pp && *pp != m) pp = &(*pp)->dbg_next;
    if (*pp == m) *pp = m->dbg_next;
    memobj64_unlock(flags);

    physmem64_free_pages(m->base_phys, m->npages);
    kfree(m);
}

void memobj64_add_ref(memobj64_t* m) {
    if (!m) return;
    uint64_t flags = memobj64_lock();
    m->refcount++;
    memobj64_unlock(flags);
}

void memobj64_release(memobj64_t* m) {
    if (!m) return;
    uint64_t flags = memobj64_lock();
    if (m->refcount == 0) {
        // Double-release / refcount-underflow guard: a caller bug (one
        // release call too many) must never wrap this counter back around
        // to its max value and must never free the backing a second time.
        // Logged as a real bug -- this should never legitimately happen --
        // but deliberately not fatal, matching this codebase's existing
        // "log and ignore" convention for invalid frees (see
        // physmem64_free_pages's own header comment).
        memobj64_unlock(flags);
        klog("memobj64: BUG -- release() called with refcount already 0\n");
        return;
    }
    m->refcount--;
    int should_free = (m->refcount == 0);
    memobj64_unlock(flags);
    if (should_free) unlink_and_free(m);
}

int memobj64_get_runs(memobj64_t* m, memobj64_run_t* runs_out, int max_runs) {
    if (!m) return -1;
    if (max_runs < 1) return -1; // today's only kind always has exactly 1 run
    runs_out[0].phys   = m->base_phys;
    runs_out[0].npages = m->npages;
    return 1;
}

void memobj64_dump(void) {
    uint64_t flags = memobj64_lock();
    klog("memobj64: dump ---\n");
    for (memobj64_t* m = g_memobj_list; m; m = m->dbg_next) {
        char buf[24];
        klog("  memobj: npages=");
        dec_to_str_local(m->npages, buf); klog(buf);
        klog(" bytes=");
        dec_to_str_local((uint64_t)m->npages * 0x1000ULL, buf); klog(buf);
        klog(" refcount=");
        dec_to_str_local(m->refcount, buf); klog(buf);
        klog("\n");
    }
    klog("memobj64: dump end ---\n");
    memobj64_unlock(flags);
}

// ── Self-test suite ──────────────────────────────────────────────────
static int test_create_and_release(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    memobj64_t* m;
    if (memobj64_create(0x1000, &m) < 0) return 0;
    int ok = (m->npages == 1 && m->refcount == 1 && m->base_phys != 0);

    memobj64_release(m);
    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;

    return ok;
}

static int test_multiple_references(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    memobj64_t* m;
    if (memobj64_create(2 * 0x1000ULL, &m) < 0) return 0;
    int ok = 1;

    memobj64_add_ref(m);
    memobj64_add_ref(m);
    if (m->refcount != 3) ok = 0;

    memobj64_release(m);
    if (ok && m->refcount != 2) ok = 0;
    memobj64_release(m);
    if (ok && m->refcount != 1) ok = 0;

    // Physical stats must NOT have moved yet -- the backing is still
    // referenced once (the original creating reference).
    physmem64_stats_t mid;
    physmem64_stats(&mid);
    if (before.free_pages - mid.free_pages != 2) ok = 0; // still allocated

    memobj64_release(m); // final reference -> frees
    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;

    return ok;
}

static int test_get_runs(void) {
    memobj64_t* m;
    if (memobj64_create(3 * 0x1000ULL, &m) < 0) return 0;
    int ok = 1;

    memobj64_run_t runs[4];
    int n = memobj64_get_runs(m, runs, 4);
    if (n != 1) ok = 0;
    if (ok && (runs[0].phys != m->base_phys || runs[0].npages != m->npages)) ok = 0;

    // max_runs == 0 must be rejected cleanly, not read/write out of bounds.
    if (memobj64_get_runs(m, runs, 0) != -1) ok = 0;
    // NULL object must be rejected cleanly.
    if (memobj64_get_runs(0, runs, 4) != -1) ok = 0;

    memobj64_release(m);
    return ok;
}

static int test_zero_size_rejected(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    memobj64_t* m = (memobj64_t*)1; // poison -- must never be written on failure
    int rc = memobj64_create(0, &m);
    physmem64_stats(&after);

    return rc < 0 && m == (memobj64_t*)1 &&
           before.free_pages == after.free_pages && before.used_pages == after.used_pages;
}

static int test_size_rounding_overflow_rejected(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    memobj64_t* m1 = (memobj64_t*)1;
    int rc1 = memobj64_create(0xFFFFFFFFFFFFFFFFULL, &m1); // would overflow size+0xFFF
    memobj64_t* m2 = (memobj64_t*)1;
    int rc2 = memobj64_create(0xFFFFFFFFFFFFFFFFULL - 0x800ULL, &m2); // rounds up past the uint64_t range

    physmem64_stats(&after);
    return rc1 < 0 && rc2 < 0 && m1 == (memobj64_t*)1 && m2 == (memobj64_t*)1 &&
           before.free_pages == after.free_pages && before.used_pages == after.used_pages;
}

static int test_allocation_failure_cleanup(void) {
    // Drain every free page but a handful, then ask for far more than
    // that -- physmem64_alloc_pages() must fail, and memobj64_create()
    // must leave nothing allocated behind (no leaked tracking struct,
    // no leaked pages) -- same "exact round trip" check every other
    // self-test in this codebase uses for this exact scenario (see
    // kernel/uservm64.c's test_partial_rollback_on_exhaustion).
    physmem64_stats_t stats;
    physmem64_stats(&stats);
    uint64_t drain = stats.free_pages > 2 ? stats.free_pages - 2 : 0;
    uint64_t drained = drain ? physmem64_alloc_pages(drain) : 0;
    if (drain && !drained) return 0; // couldn't set up the test

    physmem64_stats_t before;
    physmem64_stats(&before);

    memobj64_t* m;
    int rc = memobj64_create(10 * 0x1000ULL, &m); // ask for far more than the 2 pages left

    physmem64_stats_t after;
    physmem64_stats(&after);

    int ok = (rc < 0) && before.free_pages == after.free_pages && before.used_pages == after.used_pages;

    if (drained) physmem64_free_pages(drained, drain);
    return ok;
}

#define MEMOBJ64_TEST(name, expr) do {            \
    int _r = (expr);                              \
    klog("memobj64_selftest: " name " ");         \
    klog(_r ? "PASS\n" : "FAIL\n");               \
    if (_r) pass++; else fail++;                  \
} while (0)

int memobj64_selftest(void) {
    int pass = 0, fail = 0;
    klog("memobj64_selftest: starting\n");

    MEMOBJ64_TEST("create + release reclaims backing", test_create_and_release());
    MEMOBJ64_TEST("multiple references keep backing alive until the last release", test_multiple_references());
    MEMOBJ64_TEST("memobj64_get_runs reports the one contiguous run", test_get_runs());
    MEMOBJ64_TEST("zero-size create rejected, nothing allocated", test_zero_size_rejected());
    MEMOBJ64_TEST("size/page-rounding overflow rejected, nothing allocated", test_size_rounding_overflow_rejected());
    MEMOBJ64_TEST("allocation failure leaves nothing behind", test_allocation_failure_cleanup());

    memobj64_dump();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("memobj64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
