// kernel/vfs64.c — Milestone 27: VFS layer between kernel/syscall64.c
// and the actual filesystem backend (kernel/txfs64.c today). See
// include/vfs64.h's header comment for the overall design.
#include <stdint.h>
#include "../include/vfs64.h"
#include "../include/txfs64.h"
#include "../include/heap64.h"
#include "../include/process64.h"
#include "../include/klog.h"

// ── Mount table ──────────────────────────────────────────────────────
typedef struct {
    char mount_point[32];
    const vfs64_backend_t* ops;
} vfs64_mount_entry_t;

static vfs64_mount_entry_t g_mounts[VFS64_MAX_MOUNTS];
static int g_mount_count = 0;

static int str_len(const char* s) { int i = 0; while (s[i]) i++; return i; }

static int starts_with(const char* s, const char* pre) {
    int i = 0;
    while (pre[i]) { if (s[i] != pre[i]) return 0; i++; }
    return 1;
}

int vfs64_mount(const char* mount_point, const vfs64_backend_t* ops) {
    if (g_mount_count >= VFS64_MAX_MOUNTS) return -1;
    int len = str_len(mount_point);
    if (len >= (int)sizeof(g_mounts[0].mount_point)) return -1;

    g_mounts[g_mount_count].ops = ops;
    for (int i = 0; i < len; i++) g_mounts[g_mount_count].mount_point[i] = mount_point[i];
    g_mounts[g_mount_count].mount_point[len] = 0;
    g_mount_count++;
    return 0;
}

// Longest-prefix-match mount lookup -- with only "/" ever mounted this
// milestone, this always returns that one entry, but the scan itself
// is what makes adding a second, more specific mount (e.g. "/mnt/usb")
// later purely additive: register it and it naturally wins for paths
// under it, with no change to this function or to syscall64.c.
static const vfs64_backend_t* find_backend(const char* normalized_path) {
    const vfs64_backend_t* best = 0;
    int best_len = -1;
    for (int i = 0; i < g_mount_count; i++) {
        if (starts_with(normalized_path, g_mounts[i].mount_point)) {
            int mlen = str_len(g_mounts[i].mount_point);
            if (mlen > best_len) { best = g_mounts[i].ops; best_len = mlen; }
        }
    }
    return best;
}

static const vfs64_backend_t txfs64_backend = {
    .name        = "txfs64",
    .lookup      = txfs64_lookup,
    .read_at     = txfs64_read_at,
    .write_at    = txfs64_write_at,
    .readdir_at  = txfs64_readdir_at,
    .create_file = txfs64_create_file,
    .mkdir       = txfs64_mkdir,
    .unlink      = txfs64_unlink,
    .rmdir       = txfs64_rmdir,
    .rename      = txfs64_rename,
    .write_file  = txfs64_write_file,
};

int vfs64_init_root_txfs(void) {
    if (txfs64_mount() < 0) return -1;
    return vfs64_mount("/", &txfs64_backend);
}

// ── Path normalization ──────────────────────────────────────────────
#define VFS64_MAX_COMPONENTS 32

int vfs64_normalize_path(const char* in, char* out, int out_max) {
    if (!in || !out || out_max < 2) return -1;

    char comps[VFS64_MAX_COMPONENTS][VFS64_NAME_MAX];
    int ncomp = 0;

    const char* p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        int len = 0;
        while (p[len] && p[len] != '/') len++;
        if (len >= VFS64_NAME_MAX) return -1; // component too long

        if (len == 1 && p[0] == '.') {
            // no-op component
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (ncomp > 0) ncomp--; // pop; clamped at root if already empty
        } else {
            if (ncomp >= VFS64_MAX_COMPONENTS) return -1; // path too deep
            for (int i = 0; i < len; i++) comps[ncomp][i] = p[i];
            comps[ncomp][len] = 0;
            ncomp++;
        }
        p += len;
    }

    int pos = 0;
    out[pos++] = '/';
    for (int c = 0; c < ncomp; c++) {
        int clen = str_len(comps[c]);
        if (c > 0) {
            if (pos + 1 >= out_max) return -1;
            out[pos++] = '/';
        }
        if (pos + clen >= out_max) return -1;
        for (int i = 0; i < clen; i++) out[pos++] = comps[c][i];
    }
    out[pos] = 0;
    return 0;
}

// ── Node-level operations ────────────────────────────────────────────
int vfs64_lookup(const char* path, vfs64_node_t* out) {
    char norm[VFS64_PATH_MAX];
    if (vfs64_normalize_path(path, norm, sizeof(norm)) < 0) return -1;

    const vfs64_backend_t* be = find_backend(norm);
    if (!be || !be->lookup) return -1;

    uint32_t inum; uint64_t size; int is_dir;
    if (be->lookup(norm, &inum, &size, &is_dir) < 0) return -1;

    out->backend = be;
    out->inum    = inum;
    out->size    = size;
    out->is_dir  = is_dir;
    return 0;
}

int vfs64_read(const vfs64_node_t* node, uint64_t offset, uint8_t* buf, uint32_t len) {
    if (!node->backend || !node->backend->read_at) return -1;
    if (node->is_dir) return -1;
    return node->backend->read_at(node->inum, offset, buf, len);
}

int vfs64_write(vfs64_node_t* node, uint64_t offset, const uint8_t* buf, uint32_t len) {
    if (!node->backend || !node->backend->write_at) return -1;
    if (node->is_dir) return -1;
    int n = node->backend->write_at(node->inum, offset, buf, len);
    if (n > 0 && offset + (uint64_t)n > node->size) node->size = offset + (uint64_t)n;
    return n;
}

int vfs64_readdir(const vfs64_node_t* node, uint32_t index, char* name_out) {
    if (!node->backend || !node->backend->readdir_at) return -1;
    if (!node->is_dir) return -1;
    return node->backend->readdir_at(node->inum, index, name_out);
}

int vfs64_readdir_path(const char* path, uint32_t index, char* name_out) {
    vfs64_node_t node;
    if (vfs64_lookup(path, &node) < 0) return -1;
    return vfs64_readdir(&node, index, name_out);
}

int vfs64_create_file(const char* path) {
    char norm[VFS64_PATH_MAX];
    if (vfs64_normalize_path(path, norm, sizeof(norm)) < 0) return -1;
    const vfs64_backend_t* be = find_backend(norm);
    if (!be || !be->create_file) return -1;
    return be->create_file(norm);
}

int vfs64_mkdir(const char* path) {
    char norm[VFS64_PATH_MAX];
    if (vfs64_normalize_path(path, norm, sizeof(norm)) < 0) return -1;
    const vfs64_backend_t* be = find_backend(norm);
    if (!be || !be->mkdir) return -1;
    return be->mkdir(norm);
}

int vfs64_unlink(const char* path) {
    char norm[VFS64_PATH_MAX];
    if (vfs64_normalize_path(path, norm, sizeof(norm)) < 0) return -1;
    const vfs64_backend_t* be = find_backend(norm);
    if (!be || !be->unlink) return -1;
    return be->unlink(norm);
}

int vfs64_rmdir(const char* path) {
    char norm[VFS64_PATH_MAX];
    if (vfs64_normalize_path(path, norm, sizeof(norm)) < 0) return -1;
    const vfs64_backend_t* be = find_backend(norm);
    if (!be || !be->rmdir) return -1;
    return be->rmdir(norm);
}

int vfs64_rename(const char* src, const char* dest) {
    char nsrc[VFS64_PATH_MAX], ndest[VFS64_PATH_MAX];
    if (vfs64_normalize_path(src, nsrc, sizeof(nsrc)) < 0) return -1;
    if (vfs64_normalize_path(dest, ndest, sizeof(ndest)) < 0) return -1;
    const vfs64_backend_t* be = find_backend(nsrc);
    if (!be || be != find_backend(ndest) || !be->rename) return -1; // cross-backend rename unsupported
    return be->rename(nsrc, ndest);
}

int vfs64_write_file(const char* path, const uint8_t* data, uint32_t len) {
    char norm[VFS64_PATH_MAX];
    if (vfs64_normalize_path(path, norm, sizeof(norm)) < 0) return -1;
    const vfs64_backend_t* be = find_backend(norm);
    if (!be || !be->write_file) return -1;
    return be->write_file(norm, data, len);
}

// ── Open-file objects ────────────────────────────────────────────────
static vfs64_file_t* g_file_list = 0; // intrusive list of every live open-file object, diagnostics only

static inline uint64_t vfs64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void vfs64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

int vfs64_open(const char* path, vfs64_file_t** out) {
    vfs64_node_t node;
    if (vfs64_lookup(path, &node) < 0) return -1;

    vfs64_file_t* f = (vfs64_file_t*)kmalloc(sizeof(vfs64_file_t));
    if (!f) return -1;

    f->node     = node;
    f->cursor   = 0;
    f->refcount = 1;

    uint64_t flags = vfs64_lock();
    f->dbg_next = g_file_list;
    g_file_list = f;
    vfs64_unlock(flags);

    *out = f;
    return 0;
}

void vfs64_file_add_ref(vfs64_file_t* f) {
    uint64_t flags = vfs64_lock();
    f->refcount++;
    vfs64_unlock(flags);
}

void vfs64_file_release(vfs64_file_t* f) {
    uint64_t flags = vfs64_lock();
    f->refcount--;
    int should_free = (f->refcount == 0);
    if (should_free) {
        vfs64_file_t** pp = &g_file_list;
        while (*pp && *pp != f) pp = &(*pp)->dbg_next;
        if (*pp == f) *pp = f->dbg_next;
    }
    vfs64_unlock(flags);
    if (should_free) kfree(f);
}

// ── Diagnostics ──────────────────────────────────────────────────────
static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

void vfs64_dump(void) {
    klog("vfs64: dump ---\n  mounts:\n");
    for (int i = 0; i < g_mount_count; i++) {
        klog("    "); klog(g_mounts[i].mount_point);
        klog(" -> "); klog(g_mounts[i].ops->name ? g_mounts[i].ops->name : "?"); klog("\n");
    }

    uint64_t flags = vfs64_lock();
    klog("  open files:\n");
    int seq = 0;
    char buf[24];
    for (vfs64_file_t* f = g_file_list; f; f = f->dbg_next, seq++) {
        klog("    #"); dec_to_str_local((uint64_t)seq, buf); klog(buf);
        klog(": inum="); dec_to_str_local(f->node.inum, buf); klog(buf);
        klog(f->node.is_dir ? " (dir)" : " (file)");
        klog(" size="); dec_to_str_local(f->node.size, buf); klog(buf);
        klog(" cursor="); dec_to_str_local(f->cursor, buf); klog(buf);
        klog(" refcount="); dec_to_str_local(f->refcount, buf); klog(buf);
        klog("\n");
    }
    vfs64_unlock(flags);
    klog("vfs64: dump end ---\n");
}

// ── Self-test suite ──────────────────────────────────────────────────
// Standalone cases exercise vfs64.c's own logic (path normalization,
// open-file objects, node dispatch) directly, without a real process --
// kernel/txfs64.c's own selftest already covers block/indirect-write
// correctness in depth, so these don't re-test that, only that the VFS
// layering above it behaves correctly. Cases that genuinely need real,
// separate processes (inheritance, simultaneous independent opens,
// automatic cleanup on exit, mixed handle kinds) spawn
// user64/vfs_test64.c, mirroring kernel/pipe64.c's/kernel/shm64.c's own
// test_ring3_driver pattern.
#define VFS64_TEST_FILE    "/vfs64_selftest_tmp"
#define VFS64_TEST_DIR     "/vfs64_selftest_dir"
#define VFS64_TEST_DIR_REN "/vfs64_selftest_dir_renamed"
#define VFS_TEST_PROGRAM   "/vfs_test64.nex64"

static int str_len_local(const char* s) { return str_len(s); }
static int str_eq_local(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int test_normalize_path(void) {
    char out[VFS64_PATH_MAX];
    int ok = 1;

    if (vfs64_normalize_path("/", out, sizeof(out)) < 0 || str_len_local(out) != 1 || out[0] != '/') ok = 0;
    if (ok && (vfs64_normalize_path("a/b", out, sizeof(out)) < 0 || !str_eq_local(out, "/a/b"))) ok = 0;
    if (ok && (vfs64_normalize_path("//a///b//", out, sizeof(out)) < 0 || !str_eq_local(out, "/a/b"))) ok = 0;
    if (ok && (vfs64_normalize_path("/a/./b", out, sizeof(out)) < 0 || !str_eq_local(out, "/a/b"))) ok = 0;
    if (ok && (vfs64_normalize_path("/a/b/..", out, sizeof(out)) < 0 || !str_eq_local(out, "/a"))) ok = 0;
    if (ok && (vfs64_normalize_path("/a/../../b", out, sizeof(out)) < 0 || !str_eq_local(out, "/b"))) ok = 0; // ".." above root clamps, doesn't error
    if (ok && (vfs64_normalize_path("..", out, sizeof(out)) < 0 || !str_eq_local(out, "/"))) ok = 0;
    if (ok && (vfs64_normalize_path("", out, sizeof(out)) < 0 || !str_eq_local(out, "/"))) ok = 0;

    // Overflow: a component longer than VFS64_NAME_MAX must be rejected.
    if (ok) {
        char longcomp[VFS64_NAME_MAX + 10];
        for (int i = 0; i < VFS64_NAME_MAX + 9; i++) longcomp[i] = 'x';
        longcomp[VFS64_NAME_MAX + 9] = 0;
        if (vfs64_normalize_path(longcomp, out, sizeof(out)) == 0) ok = 0;
    }

    return ok;
}

static int test_open_read_existing_file(void) {
    vfs64_file_t* f;
    if (vfs64_open("/hello.ts", &f) < 0) return 0;
    int ok = (!f->node.is_dir && f->node.size > 0 && f->cursor == 0 && f->refcount == 1);

    uint8_t buf[64];
    int n = vfs64_read(&f->node, 0, buf, sizeof(buf));
    if (n <= 0) ok = 0;

    vfs64_file_release(f);
    return ok;
}

static int test_create_write_reopen_read(void) {
    if (vfs64_create_file(VFS64_TEST_FILE) < 0) return 0;
    int ok = 1;

    vfs64_file_t* wf;
    if (vfs64_open(VFS64_TEST_FILE, &wf) < 0) { vfs64_unlink(VFS64_TEST_FILE); return 0; }

    const char* msg = "vfs64 test content";
    uint32_t mlen = 19;
    if (vfs64_write(&wf->node, 0, (const uint8_t*)msg, mlen) != (int)mlen) ok = 0;
    vfs64_file_release(wf);

    vfs64_file_t* rf;
    if (ok && vfs64_open(VFS64_TEST_FILE, &rf) < 0) ok = 0;
    if (ok) {
        if (rf->cursor != 0) ok = 0; // a fresh open, independent of the writer's now-closed cursor
        uint8_t buf[32];
        int n = vfs64_read(&rf->node, rf->cursor, buf, sizeof(buf));
        if (n != (int)mlen) ok = 0;
        for (uint32_t i = 0; ok && i < mlen; i++) if (buf[i] != (uint8_t)msg[i]) ok = 0;
        vfs64_file_release(rf);
    }

    vfs64_unlink(VFS64_TEST_FILE);
    return ok;
}

static int test_large_file_via_vfs(void) {
    if (vfs64_create_file(VFS64_TEST_FILE) < 0) return 0;
    int ok = 1;

    vfs64_file_t* f;
    if (vfs64_open(VFS64_TEST_FILE, &f) < 0) { vfs64_unlink(VFS64_TEST_FILE); return 0; }

    uint32_t total = 150 * 1024; // crosses the direct/indirect boundary
    uint8_t* wbuf = (uint8_t*)kmalloc(total);
    uint8_t* rbuf = (uint8_t*)kmalloc(total);
    if (!wbuf || !rbuf) ok = 0;
    if (ok) {
        for (uint32_t i = 0; i < total; i++) wbuf[i] = (uint8_t)(i * 3 + 1);
        if (vfs64_write(&f->node, 0, wbuf, total) != (int)total) ok = 0;
        if (ok && vfs64_read(&f->node, 0, rbuf, total) != (int)total) ok = 0;
        for (uint32_t i = 0; ok && i < total; i++) if (rbuf[i] != wbuf[i]) ok = 0;
    }
    if (wbuf) kfree(wbuf);
    if (rbuf) kfree(rbuf);

    vfs64_file_release(f);
    vfs64_unlink(VFS64_TEST_FILE);
    return ok;
}

static int test_multiple_independent_opens(void) {
    if (vfs64_create_file(VFS64_TEST_FILE) < 0) return 0;
    int ok = 1;

    vfs64_file_t* a;
    vfs64_file_t* b;
    if (vfs64_open(VFS64_TEST_FILE, &a) < 0) ok = 0;
    if (ok && vfs64_open(VFS64_TEST_FILE, &b) < 0) ok = 0;
    if (ok && a == b) ok = 0; // must be two distinct objects, not a shared/reused one

    if (ok) {
        uint8_t buf[16];
        for (int i = 0; i < 16; i++) buf[i] = (uint8_t)i;
        if (vfs64_write(&a->node, a->cursor, buf, 16) != 16) ok = 0;
        a->cursor += 16;
        if (a->cursor != 16) ok = 0;
        if (b->cursor != 0) ok = 0; // b's cursor is entirely unaffected by a's read/write
    }

    if (a) vfs64_file_release(a);
    if (b) vfs64_file_release(b);
    vfs64_unlink(VFS64_TEST_FILE);
    return ok;
}

static int test_directory_enum_via_vfs(void) {
    vfs64_file_t* dir;
    if (vfs64_open("/", &dir) < 0) return 0;
    int ok = dir->node.is_dir;

    int count = 0;
    char name[VFS64_NAME_MAX];
    while (ok && vfs64_readdir(&dir->node, (uint32_t)count, name) == 0) count++;
    if (count == 0) ok = 0;

    vfs64_file_release(dir);
    return ok;
}

static int test_mkdir_rename_rmdir_via_vfs(void) {
    if (vfs64_mkdir(VFS64_TEST_DIR) < 0) return 0;
    int ok = 1;

    vfs64_node_t node;
    if (vfs64_lookup(VFS64_TEST_DIR, &node) < 0 || !node.is_dir) ok = 0;

    if (ok && vfs64_rename(VFS64_TEST_DIR, VFS64_TEST_DIR_REN) < 0) ok = 0;
    if (ok && vfs64_lookup(VFS64_TEST_DIR, &node) == 0) ok = 0; // old name must be gone
    if (ok && (vfs64_lookup(VFS64_TEST_DIR_REN, &node) < 0 || !node.is_dir)) ok = 0;

    if (ok && vfs64_rmdir(VFS64_TEST_DIR_REN) < 0) ok = 0;
    if (ok && vfs64_lookup(VFS64_TEST_DIR_REN, &node) == 0) ok = 0;

    // Best-effort cleanup regardless of where the sequence above failed.
    vfs64_rmdir(VFS64_TEST_DIR);
    vfs64_rmdir(VFS64_TEST_DIR_REN);
    return ok;
}

static int test_invalid_paths_rejected(void) {
    vfs64_node_t node;
    int ok = 1;
    if (vfs64_lookup("/this_path_does_not_exist_12345", &node) == 0) ok = 0;
    vfs64_file_t* f;
    if (vfs64_open("/this_path_does_not_exist_12345", &f) == 0) { vfs64_file_release(f); ok = 0; }
    if (vfs64_unlink("/this_path_does_not_exist_12345") == 0) ok = 0;
    if (vfs64_rmdir("/this_path_does_not_exist_12345") == 0) ok = 0;
    if (vfs64_rename("/this_path_does_not_exist_12345", "/also_missing") == 0) ok = 0;
    return ok;
}

static int test_repeated_cycles_no_leak(void) {
    heap64_stats_t before, after;
    txfs64_stats_t tbefore, tafter;
    heap64_stats(&before);
    txfs64_stats(&tbefore);

    for (int i = 0; i < 10; i++) {
        if (vfs64_create_file(VFS64_TEST_FILE) < 0) return 0;
        vfs64_file_t* f;
        if (vfs64_open(VFS64_TEST_FILE, &f) < 0) { vfs64_unlink(VFS64_TEST_FILE); return 0; }
        uint8_t buf[64];
        for (int j = 0; j < 64; j++) buf[j] = (uint8_t)(j + i);
        int wok = (vfs64_write(&f->node, 0, buf, 64) == 64);
        uint8_t rbuf[64];
        int rok = wok && vfs64_read(&f->node, 0, rbuf, 64) == 64;
        for (int j = 0; rok && j < 64; j++) if (rbuf[j] != buf[j]) { rok = 0; break; }
        vfs64_file_release(f);
        if (!wok || !rok) { vfs64_unlink(VFS64_TEST_FILE); return 0; }
        if (vfs64_unlink(VFS64_TEST_FILE) < 0) return 0;
    }

    heap64_stats(&after);
    txfs64_stats(&tafter);
    return before.used_bytes == after.used_bytes && before.span_count == after.span_count &&
           tbefore.free_blocks == tafter.free_blocks && tbefore.free_inodes == tafter.free_inodes;
}

static int test_ring3_driver(void) {
    heap64_stats_t before, after;
    heap64_stats(&before);

    uint32_t pid = 0;
    if (process64_spawn(VFS_TEST_PROGRAM, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;
    int code = process64_wait(pid);

    heap64_stats(&after);
    // Every vfs64_file_t the whole driver run created (including ones a
    // child never explicitly closed before exiting -- proving automatic
    // per-process handle cleanup on exit leaked nothing) must be gone.
    return code == 42 && before.used_bytes == after.used_bytes && before.span_count == after.span_count;
}

#define VFS64_TEST(name, expr) do {               \
    int _r = (expr);                              \
    klog("vfs64_selftest: " name " ");            \
    klog(_r ? "PASS\n" : "FAIL\n");               \
    if (_r) pass++; else fail++;                  \
} while (0)

int vfs64_selftest(void) {
    int pass = 0, fail = 0;
    klog("vfs64_selftest: starting\n");

    VFS64_TEST("path normalization", test_normalize_path());
    VFS64_TEST("open + read an existing TxFS64 file", test_open_read_existing_file());
    VFS64_TEST("create, write, reopen, read", test_create_write_reopen_read());
    VFS64_TEST("large file (crosses indirect boundary) via VFS", test_large_file_via_vfs());
    VFS64_TEST("multiple independent opens of the same file", test_multiple_independent_opens());
    VFS64_TEST("directory enumeration via VFS", test_directory_enum_via_vfs());
    VFS64_TEST("mkdir + rename + rmdir via VFS", test_mkdir_rename_rmdir_via_vfs());
    VFS64_TEST("invalid paths rejected safely", test_invalid_paths_rejected());
    VFS64_TEST("repeated create/write/read/delete cycles, no leak", test_repeated_cycles_no_leak());
    VFS64_TEST("ring3 driver: inheritance, refcount, mixed handles, cleanup on exit", test_ring3_driver());

    vfs64_dump();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("vfs64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
