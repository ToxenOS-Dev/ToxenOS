#ifndef SERVICE64_H
#define SERVICE64_H
#include <stdint.h>
#include "pipe64.h"
#include "process64.h"

// kernel/service64.c — Milestone 32: generic named local-service
// rendezvous, replacing Milestone 30's compositor-specific "encode two
// spawn-inherited handle numbers into the args string" bootstrap hack.
//
// The kernel knows nothing about compositors, windows, audio, or any
// other application-level concept -- a "service" here is just a short
// textual name one process publishes (service64_listen), a FIFO queue
// of not-yet-accepted connection requests, and the machinery to turn a
// connect/accept pair into two ordinary, independent pipe64_t objects.
// Once a connection is established, the service registry is no longer
// involved at all -- both sides just hold ordinary HANDLE64_PIPE_READ/
// WRITE handles in their own tables, exactly like Milestone 26 pipes
// always have. This is deliberate: it means "connection torn down",
// "client crashed", "server wrote past a full pipe", etc. all reuse
// kernel/pipe64.c's already-tested blocking/refcount/cleanup logic
// verbatim, with zero new transport code.
//
// Bidirectional connection model: service64_connect() creates TWO
// pipes (c2s: client writes, server reads; s2c: server writes, client
// reads) -- pipe64_create() gives each one reader-count=1/writer-count=1
// by construction. The connecting (client) process immediately gets
// c2s's WRITE end and s2c's READ end installed in its own handle table
// (both already-correct refs). The pending-connection queue node keeps
// the OTHER two ends (c2s's read end, s2c's write end) as plain
// pipe64_t* pointers -- their ref counts already correctly say 1, they
// are just not yet reachable through any process's handle table until
// service64_accept() installs them into the SERVER's table. If the
// server never accepts (it exits/crashes first), service64_unpublish()
// explicitly releases those two "reserved but never delivered" refs
// (pipe64_close_read(c2s) + pipe64_close_write(s2c)) -- exactly as if
// the server HAD accepted and then immediately closed both handles, so
// nothing ever leaks and the client (if still alive) just observes a
// normal broken-pipe/EOF on its next I/O, no special-casing needed.
//
// Lifetime: a service is owned by exactly one process's
// HANDLE64_SERVICE_LISTEN handle (include/handle64.h). Closing that
// handle -- explicitly (SYS64_HANDLE_CLOSE) or via process exit/fault
// (kernel/process64.c's close_all_handles, which already treats every
// handle kind uniformly) -- unpublishes the name immediately (a
// replacement process can service64_listen() the SAME name right
// afterward; a stale registration can never permanently block a
// restart) and drains any still-pending (not yet accepted) connections
// as described above. Already-ESTABLISHED connections are untouched by
// this -- they are just ordinary pipes at that point, cleaned up by the
// normal per-handle pipe64_close_read/write path when EACH side's own
// handle table is eventually released.

// Includes the NUL terminator. Long enough for a dotted reverse-DNS-
// style name ("tox.wm", "tox.audio", "tox.network.dhcp", ...) with
// comfortable headroom, short enough that a whole registry scan or a
// kmalloc'd service64_t stays trivially cheap.
#define SERVICE64_NAME_MAX 32

typedef struct service64_pending {
    pipe64_t* c2s; // client -> server pipe; the SERVER's future READ end
    pipe64_t* s2c; // server -> client pipe; the SERVER's future WRITE end
    struct service64_pending* next;
} service64_pending_t;

typedef struct service64 {
    char name[SERVICE64_NAME_MAX];
    uint32_t owner_pid; // diagnostics only -- ownership is really the handle table entry
    service64_pending_t* pending_head; // FIFO: oldest not-yet-accepted connection first
    service64_pending_t* pending_tail;
    uint32_t pending_count;
    struct service64* dbg_next; // intrusive global list, diagnostics + lookup
} service64_t;

// A fresh connection's two ends, from EITHER side's own point of view
// -- the SAME struct shape serves both service64_connect()'s result
// (the new client's own ends) and service64_accept()'s result (the
// server's own ends for that one client), since "my end that sends"
// and "my end that receives" is all either side ever needs to know.
// Deliberately not named around "client"/"server" so nothing here
// implies which side is which -- that is pure application policy.
typedef struct {
    int send; // handle: write end, me -> the other side
    int recv; // handle: read end, the other side -> me
} service64_endpoints_t;

void service64_init(void);

// True iff `name` is a well-formed service name: 1..SERVICE64_NAME_MAX-1
// bytes, every byte a printable, non-space ASCII character (0x21-0x7E).
// Callers (kernel/syscall64.c) are expected to have already copied the
// name into kernel memory via copy_user_cstr64 (which independently
// rejects unterminated strings, bad pointers, and anything longer than
// the buffer) -- this only judges the CONTENT of an already-safely-
// obtained C string.
int service64_name_valid(const char* name);

// Publishes `name` as a new service. Fails (-1) if a service with that
// name is already published (the second registration must fail
// cleanly -- see the Milestone 32 summary) or on allocation failure.
// Wakes every process blocked in service64_wait_for_registration() so
// each can recheck whether ITS wanted name now exists (a spurious wake
// for an unrelated name is harmless -- the caller's loop just rechecks
// and blocks again, same pattern as every other broad wake_all in this
// codebase). Returns 0 with *out set on success.
int service64_listen(const char* name, uint32_t owner_pid, service64_t** out);

// Looks up a live published service by exact name match, or NULL. Owns
// its own short critical section -- safe to call standalone OR nested
// inside a caller that already holds interrupts disabled (this
// codebase's cli-based locks never over-restore, see kernel/pipe64.c's
// header comment for the general pattern).
service64_t* service64_find_by_name(const char* name);

// Blocks the calling process (a real scheduler block -- see
// kernel/process64.c's process64_block_on) until SOME service is
// published. Must be called with interrupts already disabled by the
// caller, and the caller must recheck its own condition in a loop --
// exactly process64_block_on's own documented contract, passed through
// unchanged. Not a syscall itself; used by kernel/syscall64.c's
// sys64_service_connect to implement blocking connect without polling.
void service64_wait_for_registration(void);

// Establishes a new connection to `svc` on behalf of `caller`: creates
// two fresh pipes, installs the CALLER's own two ends directly into
// `caller`'s handle table (two free slots must be available), and
// enqueues the connection for `svc`'s owner to accept later. Returns 0
// with `out` filled (the caller's own send/recv handle numbers), or -1
// (no free handle slots, or allocation failure -- nothing is left
// allocated, enqueued, or partially installed on failure).
int service64_connect(service64_t* svc, process64_t* caller, service64_endpoints_t* out);

// Dequeues the OLDEST pending connection on `svc` (FIFO) and installs
// the SERVER's own two ends into `caller`'s handle table. Returns 0
// with `out` filled, -1 (a connection IS pending, but `caller` has
// fewer than two free handle slots -- the pending connection is left
// queued, untouched, so a retry after freeing a handle can still
// succeed), or -2 (nothing pending right now).
int service64_accept(service64_t* svc, process64_t* caller, service64_endpoints_t* out);

// Unpublishes `svc`: unlinks it from the global registry immediately
// (so a fresh service64_listen with the same name can succeed right
// afterward), drains and releases every still-pending connection (see
// this header's own top comment for exactly what that releases and
// why it can never leak), and frees `svc` itself. Called from BOTH
// sys64_handle_close and process64_exit_current's handle-table
// cleanup, exactly like every other handle kind's release.
void service64_unpublish(service64_t* svc);

// Diagnostics: logs every published service's name/owner/pending count
// via klog(). Development use only.
void service64_dump(void);

// Runs the Milestone 32 self-test suite. Logs each case and a final
// tally via klog(). Returns 1 if every case passed.
int service64_selftest(void);

#endif // SERVICE64_H
