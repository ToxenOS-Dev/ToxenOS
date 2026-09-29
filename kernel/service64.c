// kernel/service64.c — Milestone 32: generic named local-service
// registry. See include/service64.h for the full design writeup.
//
// Locking discipline matches kernel/pipe64.c/kernel/shm64.c exactly:
// a single pushfq/cli/restore-flags critical section per operation on
// the shared global list, safely nestable (an inner lock's conditional
// sti never over-restores an outer cli -- see pipe64.c's header
// comment) so functions here can freely call each other and be called
// from within kernel/syscall64.c's own already-cli'd retry loop for
// blocking connect.
#include <stdint.h>
#include "../include/service64.h"
#include "../include/heap64.h"
#include "../include/process64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

static service64_t* g_service_list = 0; // intrusive list of every published service, lookup + diagnostics
static uint8_t g_registry_chan = 0;     // wait-channel identity: "some service was just published"

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static void copy_name_local(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int str_eq_local(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static inline uint64_t service64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void service64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

void service64_init(void) {
    g_service_list = 0;
    g_registry_chan = 0;
}

int service64_name_valid(const char* name) {
    int len = 0;
    while (name[len]) len++;
    if (len == 0 || len > SERVICE64_NAME_MAX - 1) return 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= 0x20 || c == 0x7F || c > 0x7E) return 0; // no control chars, no space, no DEL, ASCII-only
    }
    return 1;
}

// Milestone 27-style helper, private to this file (mirrors
// kernel/syscall64.c's own find_free_handle -- duplicated rather than
// exported/shared since it's three lines and syscall64.c is a layer
// ABOVE this one, not a dependency of it).
static int find_free_handle_local(process64_t* p) {
    for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
        if (p->handles[i].kind == HANDLE64_UNUSED) return i;
    }
    return -1;
}

service64_t* service64_find_by_name(const char* name) {
    uint64_t flags = service64_lock();
    service64_t* found = 0;
    for (service64_t* s = g_service_list; s; s = s->dbg_next) {
        if (str_eq_local(s->name, name)) { found = s; break; }
    }
    service64_unlock(flags);
    return found;
}

int service64_listen(const char* name, uint32_t owner_pid, service64_t** out) {
    uint64_t flags = service64_lock();
    for (service64_t* s = g_service_list; s; s = s->dbg_next) {
        if (str_eq_local(s->name, name)) { service64_unlock(flags); return -1; } // duplicate name
    }

    service64_t* s = (service64_t*)kmalloc(sizeof(service64_t));
    if (!s) { service64_unlock(flags); return -1; }

    copy_name_local(s->name, name, SERVICE64_NAME_MAX);
    s->owner_pid = owner_pid;
    s->pending_head = 0;
    s->pending_tail = 0;
    s->pending_count = 0;
    s->wait_chan = 0;
    s->dbg_next = g_service_list;
    g_service_list = s;
    service64_unlock(flags);

    process64_wake_all(&g_registry_chan);
    *out = s;
    return 0;
}

void service64_wait_for_registration(void) {
    process64_block_on(&g_registry_chan);
}

int service64_connect(service64_t* svc, process64_t* caller, service64_endpoints_t* out) {
    int slot_send = find_free_handle_local(caller);
    if (slot_send < 0) return -1;
    caller->handles[slot_send].kind = HANDLE64_PIPE_WRITE; // reserve -- keeps the next scan off this slot
    int slot_recv = find_free_handle_local(caller);
    if (slot_recv < 0) { caller->handles[slot_send].kind = HANDLE64_UNUSED; return -1; }

    pipe64_t *c2s, *s2c;
    if (pipe64_create(&c2s) < 0) {
        caller->handles[slot_send].kind = HANDLE64_UNUSED;
        caller->handles[slot_recv].kind = HANDLE64_UNUSED;
        return -1;
    }
    if (pipe64_create(&s2c) < 0) {
        pipe64_close_read(c2s); pipe64_close_write(c2s); // drops both refs -- frees c2s
        caller->handles[slot_send].kind = HANDLE64_UNUSED;
        caller->handles[slot_recv].kind = HANDLE64_UNUSED;
        return -1;
    }

    service64_pending_t* node = (service64_pending_t*)kmalloc(sizeof(service64_pending_t));
    if (!node) {
        pipe64_close_read(c2s); pipe64_close_write(c2s);
        pipe64_close_read(s2c); pipe64_close_write(s2c);
        caller->handles[slot_send].kind = HANDLE64_UNUSED;
        caller->handles[slot_recv].kind = HANDLE64_UNUSED;
        return -1;
    }
    node->c2s = c2s;
    node->s2c = s2c;
    node->next = 0;

    // The caller's own ends: writer of c2s, reader of s2c. Both pipes
    // were just created with readers=1/writers=1 -- these installs
    // consume exactly the "half" that belongs to the connecting side;
    // the other half (c2s's reader, s2c's writer) rides along inside
    // `node` until service64_accept (or service64_unpublish, if the
    // server never gets there) claims it.
    caller->handles[slot_send].obj = c2s;
    caller->handles[slot_recv].kind = HANDLE64_PIPE_READ;
    caller->handles[slot_recv].obj = s2c;

    uint64_t flags = service64_lock();
    if (svc->pending_tail) svc->pending_tail->next = node;
    else svc->pending_head = node;
    svc->pending_tail = node;
    svc->pending_count++;
    service64_unlock(flags);

    // M+12B: wake anyone blocked (via WAIT_ANY) waiting for a new
    // connection on this listener -- see service64_t::wait_chan's own
    // header comment. Before this, service64_accept was ALWAYS polled;
    // nothing ever woke on a fresh pending connection at all.
    process64_wake_all(&svc->wait_chan);

    out->send = slot_send;
    out->recv = slot_recv;
    return 0;
}

int service64_has_pending(service64_t* svc) {
    uint64_t flags = service64_lock();
    int has = (svc->pending_head != 0);
    service64_unlock(flags);
    return has;
}

void* service64_wait_chan(service64_t* svc) { return &svc->wait_chan; }

int service64_accept(service64_t* svc, process64_t* caller, service64_endpoints_t* out) {
    uint64_t flags = service64_lock();
    if (!svc->pending_head) { service64_unlock(flags); return -2; }
    service64_unlock(flags);

    int slot_recv = find_free_handle_local(caller);
    if (slot_recv < 0) return -1;
    caller->handles[slot_recv].kind = HANDLE64_PIPE_READ; // reserve
    int slot_send = find_free_handle_local(caller);
    if (slot_send < 0) { caller->handles[slot_recv].kind = HANDLE64_UNUSED; return -1; }

    flags = service64_lock();
    service64_pending_t* node = svc->pending_head;
    if (!node) {
        // Raced with another acceptor on the SAME listener (only
        // possible if two processes somehow shared one listen handle --
        // never happens via this milestone's single-owner model, kept
        // as a safe fallback rather than an assumed-impossible case).
        service64_unlock(flags);
        caller->handles[slot_recv].kind = HANDLE64_UNUSED;
        caller->handles[slot_send].kind = HANDLE64_UNUSED;
        return -2;
    }
    svc->pending_head = node->next;
    if (!svc->pending_head) svc->pending_tail = 0;
    svc->pending_count--;
    service64_unlock(flags);

    caller->handles[slot_recv].obj = node->c2s; // server reads client requests
    caller->handles[slot_send].kind = HANDLE64_PIPE_WRITE;
    caller->handles[slot_send].obj = node->s2c; // server writes events to client
    kfree(node);

    out->recv = slot_recv;
    out->send = slot_send;
    return 0;
}

static void unlink_local(service64_t* s) {
    uint64_t flags = service64_lock();
    service64_t** pp = &g_service_list;
    while (*pp && *pp != s) pp = &(*pp)->dbg_next;
    if (*pp == s) *pp = s->dbg_next;
    service64_unlock(flags);
}

void service64_unpublish(service64_t* svc) {
    unlink_local(svc); // gone from lookup immediately -- a replacement can register the same name right away

    uint64_t flags = service64_lock();
    service64_pending_t* node = svc->pending_head;
    svc->pending_head = 0;
    svc->pending_tail = 0;
    svc->pending_count = 0;
    service64_unlock(flags);

    while (node) {
        service64_pending_t* next = node->next;
        // Release the "reserved but never delivered" server-side refs
        // -- see this file's header comment for why this can never
        // leak a pipe object regardless of whether the client is still
        // alive.
        pipe64_close_read(node->c2s);
        pipe64_close_write(node->s2c);
        kfree(node);
        node = next;
    }
    kfree(svc);
}

void service64_dump(void) {
    uint64_t flags = service64_lock();
    klog("service64: dump ---\n");
    for (service64_t* s = g_service_list; s; s = s->dbg_next) {
        char buf[24];
        klog("  service: name=\"");
        klog(s->name);
        klog("\" owner_pid=");
        dec_to_str_local(s->owner_pid, buf); klog(buf);
        klog(" pending=");
        dec_to_str_local(s->pending_count, buf); klog(buf);
        klog("\n");
    }
    klog("service64: dump end ---\n");
    service64_unlock(flags);
}

// ── Self-test suite ──────────────────────────────────────────────────
// Standalone cases exercise this file's own API directly against
// synthetic process64_t values living on the stack -- valid because
// service64_connect/accept only ever touch caller->handles[] and
// caller is never dereferenced for anything else (no pid/as/vm access
// happens in this file). Cases needing REAL cross-process blocking
// (a genuine connect() that blocks until a LATER process publishes the
// name, real syscalls end to end) spawn user64/service_test64.c,
// mirroring kernel/pipe64.c's own test_ring3_driver precedent.

static process64_t* alloc_fake_process(void) {
    process64_t* p = (process64_t*)kmalloc(sizeof(process64_t));
    if (!p) return 0;
    for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
        p->handles[i].kind = HANDLE64_UNUSED;
        p->handles[i].obj = 0;
    }
    return p;
}

static int test_listen_connect_accept_basic(void) {
    service64_t* svc;
    if (service64_listen("svc.test.basic", 1, &svc) < 0) return 0;
    int ok = 1;

    process64_t* client = alloc_fake_process();
    process64_t* server = alloc_fake_process();
    if (!client || !server) ok = 0;

    service64_endpoints_t cep, sep;
    if (ok && service64_connect(svc, client, &cep) < 0) ok = 0;
    if (ok && svc->pending_count != 1) ok = 0;
    if (ok && service64_accept(svc, server, &sep) < 0) ok = 0;
    if (ok && svc->pending_count != 0) ok = 0;

    // Real bidirectional traffic over the freshly-established pipes.
    if (ok) {
        pipe64_t* c2s = (pipe64_t*)client->handles[cep.send].obj;
        pipe64_t* s2c = (pipe64_t*)client->handles[cep.recv].obj;
        uint8_t buf[8];
        if (pipe64_write(c2s, (const uint8_t*)"ping", 4) != 4) ok = 0;
        if (ok && pipe64_read((pipe64_t*)server->handles[sep.recv].obj, buf, 4) != 4) ok = 0;
        if (ok && (buf[0] != 'p' || buf[3] != 'g')) ok = 0;
        if (ok && pipe64_write((pipe64_t*)server->handles[sep.send].obj, (const uint8_t*)"pong", 4) != 4) ok = 0;
        if (ok && pipe64_read(s2c, buf, 4) != 4) ok = 0;
        if (ok && (buf[0] != 'p' || buf[3] != 'g')) ok = 0;
    }

    if (client) {
        pipe64_close_write((pipe64_t*)client->handles[cep.send].obj);
        pipe64_close_read((pipe64_t*)client->handles[cep.recv].obj);
        kfree(client);
    }
    if (server) {
        pipe64_close_read((pipe64_t*)server->handles[sep.recv].obj);
        pipe64_close_write((pipe64_t*)server->handles[sep.send].obj);
        kfree(server);
    }
    service64_unpublish(svc);
    return ok;
}

static int test_duplicate_registration_rejected(void) {
    service64_t* a;
    if (service64_listen("svc.test.dup", 1, &a) < 0) return 0;
    service64_t* b;
    int ok = (service64_listen("svc.test.dup", 2, &b) < 0); // must fail
    service64_unpublish(a);
    return ok;
}

static int test_connect_nonexistent_fails_cleanly(void) {
    return service64_find_by_name("svc.test.does.not.exist") == 0;
}

static int test_unpublish_allows_reregistration(void) {
    service64_t* a;
    if (service64_listen("svc.test.restart", 1, &a) < 0) return 0;
    service64_unpublish(a);
    service64_t* b;
    int ok = (service64_listen("svc.test.restart", 2, &b) == 0);
    if (ok) service64_unpublish(b);
    return ok;
}

static int test_name_validation(void) {
    int ok = 1;
    if (service64_name_valid("")) ok = 0;                 // empty
    if (service64_name_valid("has space")) ok = 0;         // embedded space
    if (service64_name_valid("tab\tchar")) ok = 0;         // control char
    if (!service64_name_valid("tox.wm")) ok = 0;           // valid
    if (!service64_name_valid("a")) ok = 0;                // minimal valid
    char toolong[SERVICE64_NAME_MAX + 8];
    for (int i = 0; i < SERVICE64_NAME_MAX + 4; i++) toolong[i] = 'x';
    toolong[SERVICE64_NAME_MAX + 4] = 0;
    if (service64_name_valid(toolong)) ok = 0;             // oversized
    return ok;
}

static int test_unaccepted_connection_cleanup_no_leak(void) {
    physmem64_stats_t before, after;
    heap64_stats_t hbefore, hafter;
    physmem64_stats(&before);
    heap64_stats(&hbefore);

    service64_t* svc;
    if (service64_listen("svc.test.noaccept", 1, &svc) < 0) return 0;
    process64_t* client = alloc_fake_process();
    int ok = client != 0;

    service64_endpoints_t cep;
    if (ok && service64_connect(svc, client, &cep) < 0) ok = 0;
    // Server never accepts -- unpublish must release the "phantom" refs
    // (c2s's read end, s2c's write end) without touching the client's
    // own still-open handles.
    if (ok) service64_unpublish(svc);

    // Client's own ends must still work (or at least be safely
    // closeable) even though the peer never showed up.
    if (ok) {
        pipe64_close_write((pipe64_t*)client->handles[cep.send].obj);
        pipe64_close_read((pipe64_t*)client->handles[cep.recv].obj);
    }
    if (client) kfree(client);

    physmem64_stats(&after);
    heap64_stats(&hafter);
    if (!ok) return 0;
    return after.used_pages == before.used_pages && after.free_pages == before.free_pages &&
           hafter.used_bytes == hbefore.used_bytes && hafter.span_count == hbefore.span_count;
}

static int test_repeated_cycles_no_leak(void) {
    physmem64_stats_t before, after;
    heap64_stats_t hbefore, hafter;
    physmem64_stats(&before);
    heap64_stats(&hbefore);

    for (int i = 0; i < 25; i++) {
        service64_t* svc;
        if (service64_listen("svc.test.cycle", 1, &svc) < 0) return 0;
        process64_t* client = alloc_fake_process();
        process64_t* server = alloc_fake_process();
        if (!client || !server) return 0;

        service64_endpoints_t cep, sep;
        if (service64_connect(svc, client, &cep) < 0) return 0;
        if (service64_accept(svc, server, &sep) < 0) return 0;

        pipe64_close_write((pipe64_t*)client->handles[cep.send].obj);
        pipe64_close_read((pipe64_t*)client->handles[cep.recv].obj);
        pipe64_close_read((pipe64_t*)server->handles[sep.recv].obj);
        pipe64_close_write((pipe64_t*)server->handles[sep.send].obj);
        kfree(client);
        kfree(server);
        service64_unpublish(svc);
    }

    physmem64_stats(&after);
    heap64_stats(&hafter);
    return after.used_pages == before.used_pages && after.free_pages == before.free_pages &&
           hafter.used_bytes == hbefore.used_bytes && hafter.span_count == hbefore.span_count;
}

static int test_multiple_simultaneous_connections(void) {
    service64_t* svc;
    if (service64_listen("svc.test.multi", 1, &svc) < 0) return 0;
    int ok = 1;

    #define N 4
    process64_t* clients[N];
    service64_endpoints_t ceps[N];
    for (int i = 0; i < N; i++) {
        clients[i] = alloc_fake_process();
        if (!clients[i] || service64_connect(svc, clients[i], &ceps[i]) < 0) ok = 0;
    }
    if (ok && svc->pending_count != N) ok = 0;

    process64_t* server = alloc_fake_process();
    service64_endpoints_t seps[N];
    for (int i = 0; ok && i < N; i++) {
        if (service64_accept(svc, server, &seps[i]) < 0) ok = 0;
    }
    if (ok && svc->pending_count != 0) ok = 0;

    // Each client's traffic must reach the matching server-side pair
    // (FIFO order), not get cross-wired between connections.
    for (int i = 0; ok && i < N; i++) {
        uint8_t byte = (uint8_t)(0x40 + i);
        pipe64_write((pipe64_t*)clients[i]->handles[ceps[i].send].obj, &byte, 1);
        uint8_t got = 0;
        if (pipe64_read((pipe64_t*)server->handles[seps[i].recv].obj, &got, 1) != 1 || got != byte) ok = 0;
    }

    for (int i = 0; i < N; i++) {
        if (!clients[i]) continue;
        pipe64_close_write((pipe64_t*)clients[i]->handles[ceps[i].send].obj);
        pipe64_close_read((pipe64_t*)clients[i]->handles[ceps[i].recv].obj);
        kfree(clients[i]);
    }
    if (server) {
        for (int i = 0; i < N; i++) {
            pipe64_close_read((pipe64_t*)server->handles[seps[i].recv].obj);
            pipe64_close_write((pipe64_t*)server->handles[seps[i].send].obj);
        }
        kfree(server);
    }
    #undef N
    service64_unpublish(svc);
    return ok;
}

// ── Real ring3 cross-process case ────────────────────────────────────
// Same rationale as kernel/pipe64.c's/kernel/shm64.c's own
// test_ring3_driver: genuine cross-process blocking connect (a real
// process calling the real syscall BEFORE the service exists, blocking
// until a second real process registers it) can only be proven with
// two actual scheduled processes. See user64/service_test64.c.
#define SERVICE_TEST_PROGRAM "/service_test64.nex64"

static int test_ring3_driver(void) {
    uint32_t pid = 0;
    if (process64_spawn(SERVICE_TEST_PROGRAM, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;
    return process64_wait(pid) == 42;
}

#define SERVICE64_TEST(name, expr) do {           \
    int _r = (expr);                              \
    klog("service64_selftest: " name " ");        \
    klog(_r ? "PASS\n" : "FAIL\n");                \
    if (_r) pass++; else fail++;                  \
} while (0)

int service64_selftest(void) {
    int pass = 0, fail = 0;
    klog("service64_selftest: starting\n");

    SERVICE64_TEST("listen + connect + accept, real bidirectional traffic", test_listen_connect_accept_basic());
    SERVICE64_TEST("duplicate registration rejected", test_duplicate_registration_rejected());
    SERVICE64_TEST("connect to nonexistent service fails cleanly", test_connect_nonexistent_fails_cleanly());
    SERVICE64_TEST("unpublish allows immediate re-registration", test_unpublish_allows_reregistration());
    SERVICE64_TEST("service name validation", test_name_validation());
    SERVICE64_TEST("unaccepted connection cleanup, no leak", test_unaccepted_connection_cleanup_no_leak());
    SERVICE64_TEST("repeated listen/connect/accept/close cycles, no leak", test_repeated_cycles_no_leak());
    SERVICE64_TEST("multiple simultaneous connections, correctly distinguished", test_multiple_simultaneous_connections());
    SERVICE64_TEST("ring3 driver: real blocking connect across two processes", test_ring3_driver());

    service64_dump();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("service64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
