// kernel/shm64.c — Milestone 26: reference-counted shared-memory
// objects, independent of any single process's mapping of them.
//
// See include/shm64.h for the ownership model (refcount = open
// handles + active mappings, uniformly). kernel/uservm64.c is the only
// caller of shm64_add_ref()/shm64_release() from the MAPPING side
// (uservm64_map_shm/uservm64_munmap); kernel/process64.c is the only
// caller from the HANDLE side (spawn inheritance / process exit).
//
// M+1A: the physical backing itself is now owned by memobj64_t (see
// include/memobj64.h) -- this file only manages the shm64_t VIEW
// around one (creation, the token lookup table, and this view's own
// refcount), delegating every physical-page concern to memobj64_create/
// memobj64_release. No external behavior changes; see include/shm64.h's
// updated header comment for the full rationale.
#include <stdint.h>
#include "../include/shm64.h"
#include "../include/memobj64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/uservm64.h"
#include "../include/process64.h"
#include "../include/klog.h"

static shm64_t* g_shm_list = 0; // intrusive list of every live shm object, diagnostics only
static uint64_t g_next_token = 1; // 0 is never a valid token

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static inline uint64_t shm64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void shm64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

int shm64_create(uint64_t size, shm64_t** out) {
    memobj64_t* obj;
    if (memobj64_create(size, &obj) < 0) return -1;

    shm64_t* s = (shm64_t*)kmalloc(sizeof(shm64_t));
    if (!s) { memobj64_release(obj); return -1; }

    s->obj = obj; // this view's one, permanent reference to its backing
    s->refcount = 1;

    uint64_t flags = shm64_lock();
    s->token = g_next_token++;
    s->dbg_next = g_shm_list;
    g_shm_list = s;
    shm64_unlock(flags);

    *out = s;
    return 0;
}

shm64_t* shm64_find_by_token(uint64_t token) {
    uint64_t flags = shm64_lock();
    shm64_t* found = 0;
    for (shm64_t* s = g_shm_list; s; s = s->dbg_next) {
        if (s->token == token) { found = s; break; }
    }
    shm64_unlock(flags);
    return found;
}

static void unlink_and_free(shm64_t* s) {
    uint64_t flags = shm64_lock();
    shm64_t** pp = &g_shm_list;
    while (*pp && *pp != s) pp = &(*pp)->dbg_next;
    if (*pp == s) *pp = s->dbg_next;
    shm64_unlock(flags);

    memobj64_release(s->obj); // drop this view's one reference to the backing
    kfree(s);
}

void shm64_add_ref(shm64_t* s) {
    uint64_t flags = shm64_lock();
    s->refcount++;
    shm64_unlock(flags);
}

void shm64_release(shm64_t* s) {
    uint64_t flags = shm64_lock();
    s->refcount--;
    int should_free = (s->refcount == 0);
    shm64_unlock(flags);
    if (should_free) unlink_and_free(s);
}

void shm64_dump(void) {
    uint64_t flags = shm64_lock();
    klog("shm64: dump ---\n");
    for (shm64_t* s = g_shm_list; s; s = s->dbg_next) {
        char buf[24];
        klog("  shm: npages=");
        dec_to_str_local(s->obj->npages, buf); klog(buf);
        klog(" bytes=");
        dec_to_str_local((uint64_t)s->obj->npages * 0x1000ULL, buf); klog(buf);
        klog(" refcount=");
        dec_to_str_local(s->refcount, buf); klog(buf);
        klog("\n");
    }
    klog("shm64: dump end ---\n");
    shm64_unlock(flags);
}

// ── Self-test suite ──────────────────────────────────────────────────
// Standalone cases build a private paging64_as_t/uservm64_state_t pair
// directly (Milestone 25's uservm64_selftest precedent) to exercise
// shm64.c + kernel/uservm64.c's UVM64_REGION_SHM plumbing with full
// precision, without needing a real process. Cases that genuinely need
// TWO independent address spaces (real cross-process sharing,
// isolation-of-unmap, read-only enforcement observed via a real #PF)
// spawn user64/shm_test64.c, mirroring kernel/pipe64.c's own
// test_ring3_driver.
static int setup_as(paging64_as_t* as) {
    return paging64_create_as(as) == 0;
}

static int page_is_all(paging64_as_t* as, uint64_t va, uint8_t val) {
    uint64_t phys = paging64_translate(as, va);
    if (!phys) return 0;
    uint8_t* p = (uint8_t*)physmem64_to_virt(phys);
    for (int i = 0; i < 4096; i++) if (p[i] != val) return 0;
    return 1;
}

static int test_create_and_refcount(void) {
    // Snapshotted BEFORE any allocation at all, compared for EXACT
    // equality once everything is released at the end -- not a "+1
    // page" delta assumption mid-sequence, since kfree()'ing the
    // shm64_t struct's own kernel-heap metadata can incidentally
    // trigger a heap64 span shrink (a separate, independently-tested,
    // entirely correct physmem64_free_pages call of its own -- see
    // kernel/heap64.c) that would otherwise be mistaken for a shm64.c
    // bug. Only a full round trip back to the SAME starting point is a
    // meaningful leak check here (same principle as every other
    // self-test's "repeated cycles" case in this codebase).
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) return 0;
    int ok = (s->obj->npages == 1 && s->refcount == 1);

    shm64_add_ref(s);
    if (s->refcount != 2) ok = 0;
    shm64_release(s);
    if (s->refcount != 1) ok = 0;

    shm64_release(s); // refcount -> 0, object freed
    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;

    return ok;
}

static int test_zero_filled_and_multipage(void) {
    shm64_t* s;
    if (shm64_create(3 * 0x1000ULL, &s) < 0) return 0;
    int ok = (s->obj->npages == 3);

    // physmem64_alloc_pages() zero-fills on allocation -- verify all 3
    // pages, then write a distinct pattern into each and read it back
    // through the SAME direct kernel mapping (proves multi-page objects
    // really do own that many independently addressable pages).
    for (uint32_t i = 0; ok && i < 3; i++) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(s->obj->base_phys + (uint64_t)i * 0x1000ULL);
        for (int b = 0; b < 4096; b++) if (p[b] != 0) { ok = 0; break; }
    }
    for (uint32_t i = 0; ok && i < 3; i++) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(s->obj->base_phys + (uint64_t)i * 0x1000ULL);
        for (int b = 0; b < 4096; b++) p[b] = (uint8_t)(i * 10 + 1);
    }
    for (uint32_t i = 0; ok && i < 3; i++) {
        uint8_t* p = (uint8_t*)physmem64_to_virt(s->obj->base_phys + (uint64_t)i * 0x1000ULL);
        for (int b = 0; b < 4096; b++) if (p[b] != (uint8_t)(i * 10 + 1)) { ok = 0; break; }
    }

    shm64_release(s);
    return ok;
}

static int test_map_unmap_via_uservm64(void) {
    paging64_as_t as;
    if (!setup_as(&as)) return 0;
    uservm64_state_t vm;
    uservm64_init(&vm);

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) { paging64_destroy_as(&as); return 0; }
    int ok = 1;

    uint64_t addr;
    if (uservm64_map_shm(&vm, &as, s, 1, &addr) < 0) ok = 0;
    if (ok && s->refcount != 2) ok = 0; // handle's own ref (1) + this mapping's ref (1)
    if (ok && paging64_translate(&as, addr) != s->obj->base_phys) ok = 0;
    if (ok && !page_is_all(&as, addr, 0)) ok = 0; // freshly exposed -- must read zero

    if (ok && uservm64_munmap(&vm, &as, addr, 0x1000) < 0) ok = 0;
    if (ok && paging64_is_mapped(&as, addr)) ok = 0;
    if (ok && s->refcount != 1) ok = 0; // mapping's ref dropped; handle's ref remains

    shm64_release(s); // drop the "handle" reference too
    uservm64_teardown(&vm, &as);
    paging64_destroy_as(&as);
    return ok;
}

static int test_unmap_one_keeps_alive_for_other(void) {
    paging64_as_t as1, as2;
    if (!setup_as(&as1)) return 0;
    if (!setup_as(&as2)) { paging64_destroy_as(&as1); return 0; }
    uservm64_state_t vm1, vm2;
    uservm64_init(&vm1);
    uservm64_init(&vm2);

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) { paging64_destroy_as(&as1); paging64_destroy_as(&as2); return 0; }
    int ok = 1;

    uint64_t addr1, addr2;
    if (uservm64_map_shm(&vm1, &as1, s, 1, &addr1) < 0) ok = 0;
    if (ok && uservm64_map_shm(&vm2, &as2, s, 1, &addr2) < 0) ok = 0;
    if (ok && s->refcount != 3) ok = 0; // handle + 2 mappings

    // Write through AS1's mapping, unmap AS1, then verify AS2's mapping
    // (a DIFFERENT address space, possibly a different VA) still sees
    // the write and the physical page is still very much alive.
    if (ok) { uint8_t* p = (uint8_t*)physmem64_to_virt(s->obj->base_phys); p[0] = 0xAB; }
    if (ok && uservm64_munmap(&vm1, &as1, addr1, 0x1000) < 0) ok = 0;
    if (ok && s->refcount != 2) ok = 0;

    uint64_t phys2 = ok ? paging64_translate(&as2, addr2) : 0;
    if (ok && (!phys2 || *(uint8_t*)physmem64_to_virt(phys2) != 0xAB)) ok = 0;

    if (ok && uservm64_munmap(&vm2, &as2, addr2, 0x1000) < 0) ok = 0;
    if (ok && s->refcount != 1) ok = 0;

    shm64_release(s);
    uservm64_teardown(&vm1, &as1);
    uservm64_teardown(&vm2, &as2);
    paging64_destroy_as(&as1);
    paging64_destroy_as(&as2);
    return ok;
}

static int test_readonly_perms_enforced(void) {
    paging64_as_t as;
    if (!setup_as(&as)) return 0;
    uservm64_state_t vm;
    uservm64_init(&vm);

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) { paging64_destroy_as(&as); return 0; }
    int ok = 1;

    uint64_t addr;
    if (uservm64_map_shm(&vm, &as, s, 0 /* read-only */, &addr) < 0) ok = 0;
    if (ok && (paging64_get_perms(&as, addr) & PAGING64_WRITE)) ok = 0;
    // The exact mechanism include/uservm64.h's syscall-facing consumers
    // rely on: paging64_check_user_range's need_write parameter, the
    // same check copy_to_user64 uses for EVERY syscall that writes into
    // a user buffer.
    if (ok && paging64_check_user_range(&as, addr, 8, /*need_write=*/1) == 0) ok = 0;
    if (ok && paging64_check_user_range(&as, addr, 8, /*need_write=*/0) != 0) ok = 0;

    if (ok) uservm64_munmap(&vm, &as, addr, 0x1000);
    shm64_release(s);
    uservm64_teardown(&vm, &as);
    paging64_destroy_as(&as);
    return ok;
}

static int test_multiple_objects_simultaneously(void) {
    // See test_create_and_refcount's comment on why this is snapshotted
    // before any allocation and compared for exact equality at the end,
    // not a "+4 pages" delta assumption mid-sequence.
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    shm64_t *a, *b, *c;
    if (shm64_create(0x1000, &a) < 0) return 0;
    if (shm64_create(2 * 0x1000ULL, &b) < 0) { shm64_release(a); return 0; }
    if (shm64_create(0x1000, &c) < 0) { shm64_release(a); shm64_release(b); return 0; }

    int ok = (a->obj->base_phys != b->obj->base_phys && b->obj->base_phys != c->obj->base_phys && a->obj->base_phys != c->obj->base_phys);
    ok = ok && a->obj->npages == 1 && b->obj->npages == 2 && c->obj->npages == 1;

    shm64_release(a);
    shm64_release(b);
    shm64_release(c);
    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;

    return ok;
}

// M+1A: closes a pre-existing gap -- shm64_find_by_token() (the
// cross-process handoff mechanism kernel/syscall64.c's SYS64_SHM_TOKEN/
// SYS64_SHM_OPEN_TOKEN and every real compositor client's
// wm_attach_surface() depend on) had no dedicated self-test case before
// this milestone, only implicit exercise via the real graphical desktop.
// Worth adding now specifically because M+1A changed what a token
// resolves to internally (a view over a memobj64_t, not a direct
// base_phys/npages struct) -- this proves that change is invisible to
// every token-based caller.
static int test_token_lookup(void) {
    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) return 0;
    int ok = 1;

    uint64_t tok = s->token;
    if (tok == 0) ok = 0; // 0 is never a valid token

    shm64_t* found = shm64_find_by_token(tok);
    if (found != s) ok = 0;

    // A second, unrelated object must resolve to ITSELF, never to `s`
    // -- proves lookup is keyed correctly, not just "any live object".
    shm64_t* other;
    if (shm64_create(0x1000, &other) < 0) { shm64_release(s); return 0; }
    if (shm64_find_by_token(other->token) != other) ok = 0;
    if (other->token == tok) ok = 0; // tokens must be distinct

    // A token that never existed (well past both real ones, and past
    // g_next_token's own monotonic counter at this point in the test
    // run) must resolve to NULL, not to some unrelated live object.
    uint64_t bogus = tok > other->token ? tok + 1000000 : other->token + 1000000;
    if (shm64_find_by_token(bogus) != 0) ok = 0;

    shm64_release(other);
    shm64_release(s);

    // Once released (refcount -> 0, object freed), the ORIGINAL token
    // must no longer resolve to anything -- a stale token is exactly as
    // invalid as one that never existed, never a dangling pointer.
    if (shm64_find_by_token(tok) != 0) ok = 0;

    return ok;
}

static int test_repeated_cycles_no_leak(void) {
    physmem64_stats_t before, after;
    heap64_stats_t hbefore, hafter;
    physmem64_stats(&before);
    heap64_stats(&hbefore);

    for (int i = 0; i < 20; i++) {
        paging64_as_t as;
        if (!setup_as(&as)) return 0;
        uservm64_state_t vm;
        uservm64_init(&vm);

        shm64_t* s;
        if (shm64_create(2 * 0x1000ULL, &s) < 0) { paging64_destroy_as(&as); return 0; }

        uint64_t addr;
        if (uservm64_map_shm(&vm, &as, s, 1, &addr) < 0) { shm64_release(s); paging64_destroy_as(&as); return 0; }
        if (uservm64_munmap(&vm, &as, addr, 2 * 0x1000ULL) < 0) return 0;
        shm64_release(s);
        uservm64_teardown(&vm, &as);
        paging64_destroy_as(&as);
    }

    physmem64_stats(&after);
    heap64_stats(&hafter);
    return after.used_pages == before.used_pages && after.free_pages == before.free_pages &&
           hafter.used_bytes == hbefore.used_bytes && hafter.span_count == hbefore.span_count;
}

// ── Real ring3 cross-process cases ───────────────────────────────────
// Same rationale as kernel/pipe64.c's test_ring3_driver: genuine
// cross-process sharing (two REAL address spaces, real syscalls, one
// process's write observed by another) can only be proven with two
// actual processes. user64/shm_test64.c is a self-contained driver
// that creates the shared object, spawns a child which inherits the
// handle, and checks everything itself -- see that file for the exact
// protocol.
#define SHM_TEST_PROGRAM "/shm_test64.nex64"

static int test_ring3_driver(void) {
    uint32_t pid = 0;
    if (process64_spawn(SHM_TEST_PROGRAM, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;
    return process64_wait(pid) == 42;
}

#define SHM64_TEST(name, expr) do {              \
    int _r = (expr);                             \
    klog("shm64_selftest: " name " ");           \
    klog(_r ? "PASS\n" : "FAIL\n");              \
    if (_r) pass++; else fail++;                 \
} while (0)

int shm64_selftest(void) {
    int pass = 0, fail = 0;
    klog("shm64_selftest: starting\n");

    SHM64_TEST("create + refcount + final-release reclaim", test_create_and_refcount());
    SHM64_TEST("zero-filled + multi-page addressing", test_zero_filled_and_multipage());
    SHM64_TEST("map/unmap via uservm64, refcount tracks both handle+mapping", test_map_unmap_via_uservm64());
    SHM64_TEST("unmap from one AS keeps object alive for another", test_unmap_one_keeps_alive_for_other());
    SHM64_TEST("read-only mapping rejects writes (paging64_check_user_range)", test_readonly_perms_enforced());
    SHM64_TEST("multiple shm objects alive simultaneously", test_multiple_objects_simultaneously());
    SHM64_TEST("token lookup resolves correctly, and not past release", test_token_lookup());
    SHM64_TEST("repeated create/map/unmap/release cycles, no leak", test_repeated_cycles_no_leak());
    SHM64_TEST("ring3 driver: real cross-process sharing + isolation", test_ring3_driver());

    shm64_dump();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("shm64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
