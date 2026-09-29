// kernel/gpu64.c — M+1B: generic GPU device registry + memobj64-backed
// buffer objects. See include/gpu64.h for the full design rationale
// (why gpu64_context_t and SYS64_GPU_BUFFER_MAP are deliberately not
// here yet, and why devices/buffers follow two different lifetime
// patterns borrowed from kernel/blockdev64.c and kernel/shm64.c
// respectively).
#include <stdint.h>
#include "../include/gpu64.h"
#include "../include/memobj64.h"
#include "../include/shm64.h"
#include "../include/physmem64.h"
#include "../include/paging64.h"
#include "../include/uservm64.h"
#include "../include/heap64.h"
#include "../include/klog.h"

// ── Device registry -- mirrors kernel/blockdev64.c exactly ──────────
// Unlocked: every real driver registers its device(s) during boot,
// before physmem64_map_mmio's own header comment's "before the first
// process64_spawn()" cutoff -- there is no concurrent registration to
// race against, exactly like blockdev64's registry.
static gpu64_device_t* g_gpu_head = 0;

static int str_eq_local(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
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

gpu64_device_t* gpu64_find(const char* name) {
    for (gpu64_device_t* d = g_gpu_head; d; d = d->next) {
        if (str_eq_local(d->name, name)) return d;
    }
    return 0;
}

int gpu64_register(gpu64_device_t* dev) {
    if (gpu64_find(dev->name)) {
        klog("gpu64: register: duplicate name '");
        klog(dev->name);
        klog("' -- refused\n");
        return -1;
    }
    dev->next = g_gpu_head;
    g_gpu_head = dev;

    klog("gpu64: registered '");
    klog(dev->name);
    klog("'\n");
    return 0;
}

gpu64_device_t* gpu64_iter(gpu64_device_t* prev) {
    return prev ? prev->next : g_gpu_head;
}

gpu64_device_t* gpu64_by_index(uint32_t idx) {
    gpu64_device_t* d = g_gpu_head;
    while (d && idx > 0) { d = d->next; idx--; }
    return d;
}

int gpu64_count(void) {
    int n = 0;
    for (gpu64_device_t* d = g_gpu_head; d; d = d->next) n++;
    return n;
}

void gpu64_dump(void) {
    klog("gpu64: dump ---\n");
    for (gpu64_device_t* d = g_gpu_head; d; d = d->next) {
        klog("  "); klog(d->name);
        klog(" kind=");
        char buf[24]; dec_to_str_local((uint64_t)d->kind, buf); klog(buf);
        klog("\n");
    }
    klog("gpu64: dump end ---\n");
}

// ── Buffers -- mirrors kernel/shm64.c's locked/refcounted/token style ─
static gpu64_buffer_t* g_buf_list = 0; // intrusive list of every live buffer, diagnostics only
static uint64_t g_next_buf_token = 1;   // 0 is never a valid token

static inline uint64_t gpu64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void gpu64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

// Shared by gpu64_buffer_create() and gpu64_buffer_wrap() -- everything
// downstream of "we already have a memobj64_t reference to wrap" is
// identical regardless of whether that reference came from a brand new
// memobj64_create() or from sharing someone else's existing backing.
// Takes ownership of exactly one reference to `backing`: on failure
// here, that reference is released before returning, exactly matching
// shm64_create's own kmalloc-failure unwind.
static int wrap_existing_memobj(gpu64_device_t* dev, memobj64_t* backing, uint32_t usage, gpu64_buffer_t** out) {
    if (!dev || (usage & ~GPU64_USAGE_ALL_FLAGS) != 0) { memobj64_release(backing); return -1; }

    gpu64_buffer_t* buf = (gpu64_buffer_t*)kmalloc(sizeof(gpu64_buffer_t));
    if (!buf) { memobj64_release(backing); return -1; }

    buf->backing = backing;
    buf->usage = usage;
    buf->refcount = 1;
    buf->owning_device = dev;
    buf->driver_resource_id = 0;

    if (dev->ops && dev->ops->create_buffer && dev->ops->create_buffer(dev, buf) < 0) {
        // Driver refused -- unwind exactly as if kmalloc itself had
        // failed. destroy_buffer is deliberately NOT called here: the
        // driver's own create_buffer never reported success, so it owns
        // nothing on `buf` to tear back down (mirrors the "never call
        // release on something add_ref never succeeded for" discipline
        // this whole codebase's refcounted objects share).
        memobj64_release(backing);
        kfree(buf);
        return -1;
    }

    uint64_t flags = gpu64_lock();
    buf->token = g_next_buf_token++;
    buf->dbg_next = g_buf_list;
    g_buf_list = buf;
    gpu64_unlock(flags);

    *out = buf;
    return 0;
}

int gpu64_buffer_create(gpu64_device_t* dev, uint64_t size, uint32_t usage, gpu64_buffer_t** out) {
    memobj64_t* backing;
    if (memobj64_create(size, &backing) < 0) return -1;
    return wrap_existing_memobj(dev, backing, usage, out);
}

int gpu64_buffer_wrap(gpu64_device_t* dev, memobj64_t* backing, uint32_t usage, gpu64_buffer_t** out) {
    if (!backing) return -1;
    memobj64_add_ref(backing); // this view's own, independent reference -- caller keeps theirs
    return wrap_existing_memobj(dev, backing, usage, out);
}

gpu64_buffer_t* gpu64_buffer_find_by_token(uint64_t token) {
    uint64_t flags = gpu64_lock();
    gpu64_buffer_t* found = 0;
    for (gpu64_buffer_t* b = g_buf_list; b; b = b->dbg_next) {
        if (b->token == token) { found = b; break; }
    }
    gpu64_unlock(flags);
    return found;
}

static void unlink_and_free(gpu64_buffer_t* buf) {
    uint64_t flags = gpu64_lock();
    gpu64_buffer_t** pp = &g_buf_list;
    while (*pp && *pp != buf) pp = &(*pp)->dbg_next;
    if (*pp == buf) *pp = buf->dbg_next;
    gpu64_unlock(flags);

    if (buf->owning_device->ops && buf->owning_device->ops->destroy_buffer) {
        buf->owning_device->ops->destroy_buffer(buf->owning_device, buf);
    }
    memobj64_release(buf->backing); // drop THIS view's one reference to the backing
    kfree(buf);
}

void gpu64_buffer_add_ref(gpu64_buffer_t* buf) {
    if (!buf) return;
    uint64_t flags = gpu64_lock();
    buf->refcount++;
    gpu64_unlock(flags);
}

void gpu64_buffer_release(gpu64_buffer_t* buf) {
    if (!buf) return;
    uint64_t flags = gpu64_lock();
    if (buf->refcount == 0) {
        gpu64_unlock(flags);
        klog("gpu64: BUG -- buffer release() called with refcount already 0\n");
        return;
    }
    buf->refcount--;
    int should_free = (buf->refcount == 0);
    gpu64_unlock(flags);
    if (should_free) unlink_and_free(buf);
}

void gpu64_dump_buffers(void) {
    uint64_t flags = gpu64_lock();
    klog("gpu64: buffer dump ---\n");
    for (gpu64_buffer_t* b = g_buf_list; b; b = b->dbg_next) {
        char buf[24];
        klog("  buffer: npages="); dec_to_str_local(b->backing->npages, buf); klog(buf);
        klog(" usage="); dec_to_str_local(b->usage, buf); klog(buf);
        klog(" refcount="); dec_to_str_local(b->refcount, buf); klog(buf);
        klog("\n");
    }
    klog("gpu64: buffer dump end ---\n");
    gpu64_unlock(flags);
}

// ── Null/test driver ─────────────────────────────────────────────────
// Proves the core, nothing else: never touches PCI/MMIO/VirtIO, never
// renders anything, has no real "resource" beyond an incrementing
// counter it hands back as driver_resource_id purely to prove the
// create_buffer hook actually runs. g_null_force_fail exists ONLY to
// exercise gpu64_buffer_create/wrap's failure-unwind path (test D
// below) -- a real driver has no equivalent knob.
static uint32_t g_null_next_resource_id = 1;
static int g_null_force_fail = 0;

static int null_create_buffer(gpu64_device_t* dev, gpu64_buffer_t* buf) {
    (void)dev;
    if (g_null_force_fail) return -1;
    buf->driver_resource_id = g_null_next_resource_id++;
    return 0;
}
static void null_destroy_buffer(gpu64_device_t* dev, gpu64_buffer_t* buf) {
    (void)dev; (void)buf; // nothing to do -- see this section's own header comment
}
static const gpu64_device_ops_t g_null_ops = {
    .create_buffer  = null_create_buffer,
    .destroy_buffer = null_destroy_buffer,
};
static gpu64_device_t g_null_device;
static int g_null_registered = 0;

static gpu64_device_t* ensure_null_device(void) {
    if (!g_null_registered) {
        g_null_device.name[0]='g'; g_null_device.name[1]='p'; g_null_device.name[2]='u';
        g_null_device.name[3]='-'; g_null_device.name[4]='n'; g_null_device.name[5]='u';
        g_null_device.name[6]='l'; g_null_device.name[7]='l'; g_null_device.name[8]='0';
        g_null_device.name[9]=0;
        g_null_device.kind = GPU64_DEVICE_NULL;
        g_null_device.capability_flags = 0;
        g_null_device.ops = &g_null_ops;
        g_null_device.driver_data = 0;
        if (gpu64_register(&g_null_device) < 0) return 0;
        g_null_registered = 1;
    }
    return &g_null_device;
}

// ── Self-test suite ──────────────────────────────────────────────────
static int page_all_equal(uint64_t phys, uint8_t val) {
    uint8_t* p = (uint8_t*)physmem64_to_virt(phys);
    for (int i = 0; i < 4096; i++) if (p[i] != val) return 0;
    return 1;
}

static int test_device_registry(void) {
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;
    int ok = 1;

    if (gpu64_find("gpu-null0") != dev) ok = 0;
    if (gpu64_by_index(0) != dev) ok = 0;
    if (gpu64_count() < 1) ok = 0;

    // Registering a device with the SAME name again must be refused,
    // not silently accepted as a second entry (mirrors
    // blockdev64_register's own duplicate-name behavior exactly).
    gpu64_device_t dup;
    dup.name[0]='g'; dup.name[1]='p'; dup.name[2]='u'; dup.name[3]='-';
    dup.name[4]='n'; dup.name[5]='u'; dup.name[6]='l'; dup.name[7]='l';
    dup.name[8]='0'; dup.name[9]=0;
    dup.kind = GPU64_DEVICE_NULL;
    dup.ops = &g_null_ops;
    if (gpu64_register(&dup) == 0) ok = 0;

    return ok;
}

static int test_buffer_lifecycle(void) {
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(dev, 2 * 0x1000ULL, GPU64_USAGE_CPU_WRITE | GPU64_USAGE_SCANOUT, &buf) < 0) return 0;
    int ok = (buf->backing->npages == 2 && buf->refcount == 1 && buf->owning_device == dev);
    if (ok && buf->driver_resource_id == 0) ok = 0; // proves null_create_buffer actually ran
    if (ok && !page_all_equal(buf->backing->base_phys, 0)) ok = 0; // fresh backing, zero-filled

    gpu64_buffer_release(buf);
    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;

    return ok;
}

static int test_buffer_validation(void) {
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;
    int ok = 1;
    gpu64_buffer_t* buf = (gpu64_buffer_t*)1; // poison

    if (gpu64_buffer_create(dev, 0, GPU64_USAGE_CPU_READ, &buf) == 0) ok = 0; // zero size
    if (buf != (gpu64_buffer_t*)1) ok = 0; // must not have been written

    if (gpu64_buffer_create(dev, 0x1000, 0xF0000000u, &buf) == 0) ok = 0; // unknown usage bits
    if (gpu64_buffer_create(0, 0x1000, GPU64_USAGE_CPU_READ, &buf) == 0) ok = 0; // invalid device (NULL)

    return ok;
}

static int test_token_lookup(void) {
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;
    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(dev, 0x1000, GPU64_USAGE_CPU_READ, &buf) < 0) return 0;
    int ok = 1;

    uint64_t tok = buf->token;
    if (tok == 0) ok = 0;
    if (gpu64_buffer_find_by_token(tok) != buf) ok = 0;

    gpu64_buffer_release(buf);
    if (gpu64_buffer_find_by_token(tok) != 0) ok = 0; // stale token must not resolve after release

    return ok;
}

// ── §7: the memobj64 second-consumer scenarios -- what M+1A itself
// could not exercise, since nothing but shm64 existed to reference a
// memobj64_t. Every case below has a REAL shm64_t (created through the
// real public shm64_create/shm64_release, exactly as any client
// process would) sharing its backing with a gpu64_buffer_t.
static int test_shm_release_first_gpu_survives(void) { // 7A
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) return 0;
    uint8_t* p = (uint8_t*)physmem64_to_virt(s->obj->base_phys);
    p[0] = 0x77; // a mark only the shared physical page can carry

    gpu64_buffer_t* buf;
    if (gpu64_buffer_wrap(dev, s->obj, GPU64_USAGE_SCANOUT, &buf) < 0) { shm64_release(s); return 0; }
    int ok = (buf->backing == s->obj);

    shm64_release(s); // shm64's own reference gone -- memobj must survive via the GPU buffer's reference
    if (ok && p[0] != 0x77) ok = 0; // same physical page, still readable, still correct
    if (ok && buf->backing->refcount < 1) ok = 0;

    gpu64_buffer_release(buf); // final reference -- NOW it may free
    return ok;
}

static int test_gpu_release_first_shm_survives(void) { // 7B
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) return 0;

    paging64_as_t as;
    if (paging64_create_as(&as) < 0) { shm64_release(s); return 0; }
    uservm64_state_t vm;
    uservm64_init(&vm);
    uint64_t addr;
    if (uservm64_map_shm(&vm, &as, s, 1, &addr) < 0) {
        paging64_destroy_as(&as); shm64_release(s); return 0;
    }
    int ok = 1;
    uint8_t* mapped = (uint8_t*)physmem64_to_virt(paging64_translate(&as, addr));
    mapped[0] = 0x99;

    gpu64_buffer_t* buf;
    if (gpu64_buffer_wrap(dev, s->obj, GPU64_USAGE_SCANOUT, &buf) < 0) ok = 0;

    if (ok) gpu64_buffer_release(buf); // GPU side gone FIRST -- shm's own mapping+handle must be entirely unaffected
    if (ok && paging64_translate(&as, addr) == 0) ok = 0; // mapping still live
    if (ok && mapped[0] != 0x99) ok = 0; // still the same physical page, still correct

    uservm64_munmap(&vm, &as, addr, 0x1000);
    uservm64_teardown(&vm, &as);
    paging64_destroy_as(&as);
    shm64_release(s);
    return ok;
}

static int test_survives_until_final_reference(void) { // 7C
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) return 0;
    shm64_add_ref(s); // simulates a second process's inherited handle
    gpu64_buffer_t* buf;
    if (gpu64_buffer_wrap(dev, s->obj, GPU64_USAGE_CPU_READ, &buf) < 0) { shm64_release(s); shm64_release(s); return 0; }
    int ok = (s->obj->refcount == 2); // shm64's own two "handle" refs collapse to one memobj ref; +1 for the GPU buffer's own

    physmem64_stats_t mid;
    physmem64_stats(&mid);
    if (before.free_pages == mid.free_pages) ok = 0; // still allocated -- sanity, not yet released at all

    shm64_release(s); // 1 of 2 shm handle-refs gone -- still referenced by (remaining shm handle + gpu buffer)
    physmem64_stats(&mid);
    if (before.free_pages == mid.free_pages) ok = 0; // still must be allocated

    shm64_release(s); // shm64_t itself now fully gone -- gpu buffer's own reference must still keep it alive
    physmem64_stats(&mid);
    if (before.free_pages == mid.free_pages) ok = 0; // still must be allocated

    gpu64_buffer_release(buf); // final reference anywhere -- now it may free
    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;

    return ok;
}

static int test_wrap_failure_releases_temp_ref(void) { // 7D
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;

    shm64_t* s;
    if (shm64_create(0x1000, &s) < 0) return 0;
    uint32_t before_refcount = s->obj->refcount;

    g_null_force_fail = 1;
    gpu64_buffer_t* buf = (gpu64_buffer_t*)1;
    int rc = gpu64_buffer_wrap(dev, s->obj, GPU64_USAGE_CPU_READ, &buf);
    g_null_force_fail = 0;

    int ok = (rc < 0) && (buf == (gpu64_buffer_t*)1) && (s->obj->refcount == before_refcount);
    // shm64's own object must be completely unaffected by the failed wrap.
    if (ok && !page_all_equal(s->obj->base_phys, 0)) ok = 0;

    shm64_release(s);
    return ok;
}

static int test_no_double_free_or_underflow(void) { // 7E
    // Real double-release past a genuine free is a use-after-free and
    // cannot be safely induced in a test (the memory may already be
    // reused) -- what IS safely provable is that a full, correctly
    // balanced add_ref/release sequence never leaves the refcount (or
    // the underlying physical page count) anywhere other than exactly
    // where it should be at each step, and that the defensive
    // refcount==0 guard exists in the release path (see
    // gpu64_buffer_release's own source -- exercised indirectly by
    // every other test here never tripping it unexpectedly).
    gpu64_device_t* dev = ensure_null_device();
    if (!dev) return 0;
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(dev, 0x1000, GPU64_USAGE_CPU_READ, &buf) < 0) return 0;
    gpu64_buffer_add_ref(buf);
    gpu64_buffer_add_ref(buf);
    int ok = (buf->refcount == 3);

    gpu64_buffer_release(buf);
    if (ok && buf->refcount != 2) ok = 0;
    gpu64_buffer_release(buf);
    if (ok && buf->refcount != 1) ok = 0;
    gpu64_buffer_release(buf); // final -- frees

    physmem64_stats(&after);
    if (before.free_pages != after.free_pages || before.used_pages != after.used_pages) ok = 0;
    return ok;
}

#define GPU64_TEST(name, expr) do {               \
    int _r = (expr);                              \
    klog("gpu64_selftest: " name " ");            \
    klog(_r ? "PASS\n" : "FAIL\n");               \
    if (_r) pass++; else fail++;                  \
} while (0)

int gpu64_selftest(void) {
    int pass = 0, fail = 0;
    klog("gpu64_selftest: starting\n");

    GPU64_TEST("device registry: register/find/duplicate-name refused", test_device_registry());
    GPU64_TEST("buffer create/release via null driver reclaims backing", test_buffer_lifecycle());
    GPU64_TEST("buffer creation validation (zero size, bad usage, bad device)", test_buffer_validation());
    GPU64_TEST("buffer token lookup, and not past release", test_token_lookup());
    GPU64_TEST("[7A] shm64 released first -- gpu64_buffer keeps backing alive", test_shm_release_first_gpu_survives());
    GPU64_TEST("[7B] gpu64_buffer released first -- shm64 mapping keeps backing alive", test_gpu_release_first_shm_survives());
    GPU64_TEST("[7C] backing survives until the true final reference, exactly", test_survives_until_final_reference());
    GPU64_TEST("[7D] failed wrap releases its temporary memobj reference, shm64 unaffected", test_wrap_failure_releases_temp_ref());
    GPU64_TEST("[7E] balanced add_ref/release sequence, no under/over-free", test_no_double_free_or_underflow());

    gpu64_dump();
    gpu64_dump_buffers();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("gpu64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
