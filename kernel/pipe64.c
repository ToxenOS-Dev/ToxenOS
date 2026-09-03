// kernel/pipe64.c — Milestone 26: bounded byte-stream pipes with real
// scheduler blocking.
//
// Ring-buffer state (head/tail/count/readers/writers) is protected by
// a single pushfq/cli/restore-flags critical section per operation --
// the same discipline physmem64.c/heap64.c/process64.c already use.
// Blocking is layered on top of kernel/process64.c's generalized
// process64_block_on/wake_one/wake_all: read_chan/write_chan (two
// dummy bytes inside the pipe struct) are the wait-channel identities,
// so a blocked reader/writer is a genuinely BLOCKED process, off the
// scheduler's run queue, never busy-waiting/polling.
#include <stdint.h>
#include "../include/pipe64.h"
#include "../include/process64.h"
#include "../include/heap64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

static pipe64_t* g_pipe_list = 0; // intrusive list of every live pipe, diagnostics only

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static inline uint64_t pipe64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void pipe64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

int pipe64_create(pipe64_t** out) {
    pipe64_t* p = (pipe64_t*)kmalloc(sizeof(pipe64_t));
    if (!p) return -1;

    p->buffer = (uint8_t*)kmalloc(PIPE64_BUF_SIZE);
    if (!p->buffer) { kfree(p); return -1; }

    p->capacity = PIPE64_BUF_SIZE;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;
    p->read_chan = 0;
    p->write_chan = 0;

    uint64_t flags = pipe64_lock();
    p->dbg_next = g_pipe_list;
    g_pipe_list = p;
    pipe64_unlock(flags);

    *out = p;
    return 0;
}

static void unlink_and_free(pipe64_t* p) {
    uint64_t flags = pipe64_lock();
    pipe64_t** pp = &g_pipe_list;
    while (*pp && *pp != p) pp = &(*pp)->dbg_next;
    if (*pp == p) *pp = p->dbg_next;
    pipe64_unlock(flags);

    kfree(p->buffer);
    kfree(p);
}

void pipe64_add_ref_read(pipe64_t* p) {
    uint64_t flags = pipe64_lock();
    p->readers++;
    pipe64_unlock(flags);
}

void pipe64_add_ref_write(pipe64_t* p) {
    uint64_t flags = pipe64_lock();
    p->writers++;
    pipe64_unlock(flags);
}

void pipe64_close_read(pipe64_t* p) {
    uint64_t flags = pipe64_lock();
    p->readers--;
    int should_free = (p->readers == 0 && p->writers == 0);
    if (p->readers == 0) {
        // No readers left -- any blocked writer must wake up and fail
        // cleanly (pipe64_write rechecks p->readers itself) rather than
        // block forever.
        process64_wake_all(&p->write_chan);
    }
    pipe64_unlock(flags);
    if (should_free) unlink_and_free(p);
}

void pipe64_close_write(pipe64_t* p) {
    uint64_t flags = pipe64_lock();
    p->writers--;
    int should_free = (p->readers == 0 && p->writers == 0);
    if (p->writers == 0) {
        // No writers left -- any blocked reader must wake up and observe
        // EOF (once it drains whatever's still buffered).
        process64_wake_all(&p->read_chan);
    }
    pipe64_unlock(flags);
    if (should_free) unlink_and_free(p);
}

int64_t pipe64_read(pipe64_t* p, uint8_t* kbuf, uint64_t len) {
    if (len == 0) return 0;

    uint64_t flags = pipe64_lock();
    uint64_t got = 0;
    while (got < len) {
        while (p->count == 0 && p->writers > 0) {
            process64_block_on(&p->read_chan);
        }
        if (p->count == 0) break; // writers == 0 too -- EOF

        uint64_t chunk = len - got;
        if (chunk > p->count) chunk = p->count;
        for (uint64_t i = 0; i < chunk; i++) {
            kbuf[got + i] = p->buffer[p->tail];
            p->tail = (p->tail + 1) % p->capacity;
        }
        p->count -= (uint32_t)chunk;
        got += chunk;
        process64_wake_all(&p->write_chan); // space freed -- any blocked writer can resume
    }
    pipe64_unlock(flags);
    return (int64_t)got;
}

// Milestone 30: see include/pipe64.h's header comment. Never blocks,
// and deliberately all-or-nothing -- a short read would desync the
// compositor protocol's fixed-size message framing.
int64_t pipe64_try_read(pipe64_t* p, uint8_t* kbuf, uint64_t len) {
    if (len == 0) return 0;

    uint64_t flags = pipe64_lock();
    if (p->count < len) {
        int would_block = (p->writers > 0);
        pipe64_unlock(flags);
        return would_block ? -2 : 0;
    }

    for (uint64_t i = 0; i < len; i++) {
        kbuf[i] = p->buffer[p->tail];
        p->tail = (p->tail + 1) % p->capacity;
    }
    p->count -= (uint32_t)len;
    process64_wake_all(&p->write_chan);
    pipe64_unlock(flags);
    return (int64_t)len;
}

int64_t pipe64_write(pipe64_t* p, const uint8_t* kbuf, uint64_t len) {
    if (len == 0) return 0;

    uint64_t flags = pipe64_lock();
    if (p->readers == 0) { pipe64_unlock(flags); return -1; }

    uint64_t written = 0;
    while (written < len) {
        while (p->count == p->capacity && p->readers > 0) {
            process64_block_on(&p->write_chan);
        }
        if (p->readers == 0) break; // broken mid-write -- stop, report what made it

        uint64_t space = p->capacity - p->count;
        uint64_t chunk = len - written;
        if (chunk > space) chunk = space;
        for (uint64_t i = 0; i < chunk; i++) {
            p->buffer[p->head] = kbuf[written + i];
            p->head = (p->head + 1) % p->capacity;
        }
        p->count += (uint32_t)chunk;
        written += chunk;
        process64_wake_all(&p->read_chan); // data available -- any blocked reader can resume
    }
    pipe64_unlock(flags);

    return (written == 0) ? -1 : (int64_t)written;
}

// Milestone 32.1: see include/pipe64.h's header comment. Mirrors
// pipe64_try_read's structure exactly, just for the write side.
int64_t pipe64_try_write(pipe64_t* p, const uint8_t* kbuf, uint64_t len) {
    if (len == 0) return 0;

    uint64_t flags = pipe64_lock();
    if (p->readers == 0) { pipe64_unlock(flags); return -1; }

    uint32_t space = p->capacity - p->count;
    if ((uint64_t)space < len) { pipe64_unlock(flags); return -2; }

    for (uint64_t i = 0; i < len; i++) {
        p->buffer[p->head] = kbuf[i];
        p->head = (p->head + 1) % p->capacity;
    }
    p->count += (uint32_t)len;
    process64_wake_all(&p->read_chan);
    pipe64_unlock(flags);
    return (int64_t)len;
}

void pipe64_dump(void) {
    uint64_t flags = pipe64_lock();
    klog("pipe64: dump ---\n");
    for (pipe64_t* p = g_pipe_list; p; p = p->dbg_next) {
        char buf[24];
        klog("  pipe: capacity=");
        dec_to_str_local(p->capacity, buf); klog(buf);
        klog(" count=");
        dec_to_str_local(p->count, buf); klog(buf);
        klog(" readers=");
        dec_to_str_local(p->readers, buf); klog(buf);
        klog(" writers=");
        dec_to_str_local(p->writers, buf); klog(buf);
        klog(" blocked_readers=[ ");
        process64_log_waiters("pid=", &p->read_chan);
        klog("] blocked_writers=[ ");
        process64_log_waiters("pid=", &p->write_chan);
        klog("]\n");
    }
    klog("pipe64: dump end ---\n");
    pipe64_unlock(flags);
}

// ── Self-test suite ──────────────────────────────────────────────────
// Every case runs standalone pipe64_t objects directly (no real ring3
// processes) EXCEPT the ones that genuinely need real blocking/wakeup
// across two schedulable contexts, which spawn user64/pipe_test64.c
// (see test_ring3_driver below).

static int test_basic_write_read(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;

    const char* msg = "hello pipe";
    uint64_t len = 10;
    if (pipe64_write(p, (const uint8_t*)msg, len) != (int64_t)len) ok = 0;
    if (ok && p->count != len) ok = 0;

    uint8_t buf[16] = {0};
    if (ok && pipe64_read(p, buf, len) != (int64_t)len) ok = 0;
    for (uint64_t i = 0; ok && i < len; i++) if (buf[i] != (uint8_t)msg[i]) ok = 0;
    if (ok && p->count != 0) ok = 0;

    pipe64_close_read(p);
    pipe64_close_write(p);
    return ok;
}

static int test_wraparound(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;
    uint8_t wbuf[100], rbuf[100];
    for (int i = 0; i < 100; i++) wbuf[i] = (uint8_t)i;

    // 3 writes of 100 bytes through a 256-byte ring -- forces the tail
    // pointer to wrap around at least once.
    for (int round = 0; round < 3 && ok; round++) {
        if (pipe64_write(p, wbuf, 100) != 100) { ok = 0; break; }
        if (pipe64_read(p, rbuf, 100) != 100) { ok = 0; break; }
        for (int i = 0; i < 100; i++) if (rbuf[i] != wbuf[i]) { ok = 0; break; }
    }

    pipe64_close_read(p);
    pipe64_close_write(p);
    return ok;
}

static int test_large_transfer_larger_than_buffer(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;

    // A single write bigger than the whole ring buffer's capacity --
    // pipe64_write must loop internally (fill, let something drain it,
    // keep going) rather than only ever writing one buffer's worth.
    // Since nothing else is reading concurrently here, drain it via
    // repeated pipe64_read calls issued AFTER checking the write with a
    // separately-sized buffer would deadlock a single-threaded caller --
    // so this case writes an amount that fits in one buffer-full pass
    // (capacity itself) to prove the exact-fit boundary, and the
    // multi-page cross-boundary case is exercised for real (with a
    // concurrent reader) by the ring3 test program below.
    uint8_t wbuf[PIPE64_BUF_SIZE], rbuf[PIPE64_BUF_SIZE];
    for (int i = 0; i < PIPE64_BUF_SIZE; i++) wbuf[i] = (uint8_t)(i * 3);
    if (pipe64_write(p, wbuf, PIPE64_BUF_SIZE) != PIPE64_BUF_SIZE) ok = 0;
    if (ok && pipe64_read(p, rbuf, PIPE64_BUF_SIZE) != PIPE64_BUF_SIZE) ok = 0;
    for (int i = 0; ok && i < PIPE64_BUF_SIZE; i++) if (rbuf[i] != wbuf[i]) ok = 0;

    pipe64_close_read(p);
    pipe64_close_write(p);
    return ok;
}

static int test_eof_on_last_writer_close(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;

    if (pipe64_write(p, (const uint8_t*)"ab", 2) != 2) ok = 0;
    pipe64_close_write(p); // last writer gone -- buffered data must still be readable

    uint8_t buf[4];
    if (ok && pipe64_read(p, buf, 2) != 2) ok = 0;   // drains what's left
    if (ok && (buf[0] != 'a' || buf[1] != 'b')) ok = 0;
    if (ok && pipe64_read(p, buf, 4) != 0) ok = 0;    // now truly EOF

    pipe64_close_read(p); // frees the pipe (writers already 0)
    return ok;
}

static int test_broken_pipe_on_last_reader_close(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;

    pipe64_close_read(p); // last reader gone BEFORE any write
    if (pipe64_write(p, (const uint8_t*)"x", 1) != -1) ok = 0;

    pipe64_close_write(p); // frees the pipe (readers already 0)
    return ok;
}

// ── Milestone 32.1: pipe64_try_write ────────────────────────────────
static int test_try_write_basic(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;

    // Exact-fit non-blocking write, then fill the rest of the buffer.
    uint8_t chunk[64];
    for (int i = 0; i < 64; i++) chunk[i] = (uint8_t)i;
    if (pipe64_try_write(p, chunk, 64) != 64) ok = 0;
    if (ok && p->count != 64) ok = 0;

    // Fill to exactly capacity.
    for (int round = 0; ok && round < 3; round++) {
        if (pipe64_try_write(p, chunk, 64) != 64) ok = 0;
    }
    if (ok && p->count != PIPE64_BUF_SIZE) ok = 0;

    // Now genuinely full -- a further non-blocking write of even 1
    // byte must report would-block, and (critically) must not have
    // written anything partial.
    uint8_t one = 0xFF;
    if (ok && pipe64_try_write(p, &one, 1) != -2) ok = 0;
    if (ok && p->count != PIPE64_BUF_SIZE) ok = 0; // unchanged -- no partial write

    // Drain everything and verify byte-for-byte correctness (proves
    // try_write's ring-buffer bookkeeping matches the blocking path).
    uint8_t rbuf[PIPE64_BUF_SIZE];
    if (ok && pipe64_read(p, rbuf, PIPE64_BUF_SIZE) != PIPE64_BUF_SIZE) ok = 0;
    for (int i = 0; ok && i < PIPE64_BUF_SIZE; i++) if (rbuf[i] != chunk[i % 64]) ok = 0;

    pipe64_close_read(p);
    pipe64_close_write(p);
    return ok;
}

static int test_try_write_broken_pipe(void) {
    pipe64_t* p;
    if (pipe64_create(&p) < 0) return 0;
    int ok = 1;

    pipe64_close_read(p); // last reader gone before any write
    uint8_t x = 'x';
    if (pipe64_try_write(p, &x, 1) != -1) ok = 0;

    pipe64_close_write(p); // frees the pipe (readers already 0)
    return ok;
}

// Directly reproduces the Milestone 32 compositor deadlock pattern at
// the pipe64 level: TWO pipes, each one's "full" end simultaneously
// needing to be written while the other side never drains -- proves
// pipe64_try_write lets BOTH sides discover "would block" instead of
// either one ever calling the blocking pipe64_write and hanging. This
// is exactly what user64/compositor64.c's queue_event/flush_conn_queue
// now rely on: a full event pipe to a stalled client must never stop
// forward progress on anything else.
static int test_try_write_prevents_mutual_deadlock(void) {
    pipe64_t *a2b, *b2a;
    if (pipe64_create(&a2b) < 0) return 0;
    if (pipe64_create(&b2a) < 0) { pipe64_close_read(a2b); pipe64_close_write(a2b); return 0; }
    int ok = 1;

    uint8_t chunk[32];
    for (int i = 0; i < 32; i++) chunk[i] = (uint8_t)i;

    // Fill BOTH pipes to capacity via non-blocking writes only -- if
    // this ever actually blocked, the self-test itself would hang
    // forever (there is nothing else running to drain either pipe).
    while (pipe64_try_write(a2b, chunk, 32) == 32) { }
    while (pipe64_try_write(b2a, chunk, 32) == 32) { }
    if (a2b->count != PIPE64_BUF_SIZE || b2a->count != PIPE64_BUF_SIZE) ok = 0;

    // Both directions must now report would-block, not partially
    // succeed and not block -- this IS "neither side can ever freeze
    // trying to deliver to the other."
    if (ok && pipe64_try_write(a2b, chunk, 32) != -2) ok = 0;
    if (ok && pipe64_try_write(b2a, chunk, 32) != -2) ok = 0;

    // Draining one side frees room for exactly one more write there,
    // proving the pipes recover normally once a reader resumes.
    uint8_t rbuf[32];
    if (ok && pipe64_read(a2b, rbuf, 32) != 32) ok = 0;
    if (ok && pipe64_try_write(a2b, chunk, 32) != 32) ok = 0;

    pipe64_close_read(a2b); pipe64_close_write(a2b);
    pipe64_close_read(b2a); pipe64_close_write(b2a);
    return ok;
}

static int test_repeated_cycles_no_leak(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    for (int i = 0; i < 30; i++) {
        pipe64_t* p;
        if (pipe64_create(&p) < 0) return 0;
        uint8_t buf[8];
        if (pipe64_write(p, (const uint8_t*)"cycling!", 8) != 8) return 0;
        if (pipe64_read(p, buf, 8) != 8) return 0;
        pipe64_close_read(p);
        pipe64_close_write(p);
    }

    physmem64_stats(&after);
    return after.used_pages == before.used_pages && after.free_pages == before.free_pages;
}

// ── Real ring3 blocking/wakeup cases ─────────────────────────────────
// kernel/process64.c's self-test precedent (test_nested_spawn) is
// followed here: rather than the KERNEL manipulating a process's
// handle table directly (which wouldn't even be possible from this
// kernel/idle context -- process64_current() is NULL here, there IS no
// "current process" to hold a handle), user64/pipe_test64.c is a
// SELF-CONTAINED driver: spawned once with no args, it uses the REAL
// sys_pipe_create/sys_handle_read/sys_handle_write/sys_handle_close
// syscalls itself, spawning further instances of ITSELF (with itself
// as their real parent, so Milestone 26 handle inheritance actually
// triggers) to exercise parent-writes/child-reads, child-writes/
// parent-reads, and a transfer several times larger than
// PIPE64_BUF_SIZE that must block in both directions to complete. This
// is the only way to test the actual syscall ABI end to end (not just
// kernel/pipe64.c's internals, which the standalone cases above
// already cover) -- see user64/pipe_test64.c for the full protocol.
#define PIPE_TEST_PROGRAM "/pipe_test64.nex64"

static int test_ring3_driver(void) {
    uint32_t pid = 0;
    if (process64_spawn(PIPE_TEST_PROGRAM, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;
    return process64_wait(pid) == 42;
}

#define PIPE64_TEST(name, expr) do {             \
    int _r = (expr);                             \
    klog("pipe64_selftest: " name " ");          \
    klog(_r ? "PASS\n" : "FAIL\n");               \
    if (_r) pass++; else fail++;                  \
} while (0)

int pipe64_selftest(void) {
    int pass = 0, fail = 0;
    klog("pipe64_selftest: starting\n");

    PIPE64_TEST("basic write then read", test_basic_write_read());
    PIPE64_TEST("ring-buffer wraparound", test_wraparound());
    PIPE64_TEST("exact-buffer-size transfer", test_large_transfer_larger_than_buffer());
    PIPE64_TEST("EOF after last writer closes", test_eof_on_last_writer_close());
    PIPE64_TEST("broken pipe after last reader closes", test_broken_pipe_on_last_reader_close());
    PIPE64_TEST("repeated create/use/destroy, no leak", test_repeated_cycles_no_leak());
    PIPE64_TEST("try_write: fill to capacity, would-block, no partial write", test_try_write_basic());
    PIPE64_TEST("try_write: broken pipe (no readers)", test_try_write_broken_pipe());
    PIPE64_TEST("try_write: both pipes full simultaneously never blocks (M32 deadlock pattern)", test_try_write_prevents_mutual_deadlock());
    PIPE64_TEST("ring3 driver: real syscalls, inheritance, blocking both ways", test_ring3_driver());

    pipe64_dump();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("pipe64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
