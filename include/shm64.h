#ifndef SHM64_H
#define SHM64_H

#include <stdint.h>
#include "memobj64.h"

// Milestone 26: a real kernel shared-memory object, whose lifetime is
// independent of any single process's virtual mapping of it -- see
// kernel/uservm64.c's UVM64_REGION_SHM region kind for how a process
// actually maps one of these into its own address space, and
// include/handle64.h for how userspace refers to one (never a raw
// physical address, never this struct's pointer).
//
// M+1A: this struct is now a thin VIEW over a memobj64_t (see
// include/memobj64.h), which is the actual owner of the physical
// backing and ITS OWN refcount. shm64_t no longer stores base_phys/
// npages directly -- every existing external behavior is unchanged
// (shm64_create/shm64_add_ref/shm64_release/shm64_find_by_token all
// keep their exact pre-M+1A signatures and semantics), only the
// internal representation moved. This is what lets a future
// gpu64_buffer share the SAME physical backing as an shm64 surface by
// holding its own reference to the SAME memobj64_t, without either one
// independently refcounting the same physical pages.
//
// Ownership model: `refcount` counts every outstanding REFERENCE to
// THIS shm64_t (the view), where a reference is held by EITHER an open
// per-process handle (kernel/process64.c's handle table -- one per
// HANDLE64_SHM entry) OR an active virtual mapping (kernel/uservm64.c's
// UVM64_REGION_SHM region -- one per uservm64_map_shm() call, dropped
// by the matching uservm64_munmap()). Both kinds increment/decrement
// the SAME counter uniformly. This is deliberately NOT "one reference
// per handle, implicitly covering however many times it's been
// mapped": a process can close its handle while keeping an existing
// mapping alive, or keep the handle open after unmapping -- exactly
// mirroring how a process can close a file's fd while an mmap of it
// stays valid. shm64_t itself holds exactly ONE reference to its
// memobj64_t for its entire life (taken in shm64_create(), released
// the moment this shm64_t's own refcount reaches zero) -- today that
// means the memobj's refcount is always exactly 1, since nothing yet
// shares one memobj64_t between two shm64_t views; the layering is
// what matters for M+1A, not fan-out.
typedef struct shm64 {
    memobj64_t* obj;        // owns the physical backing + its own refcount
    uint32_t refcount;      // THIS view's own refcount: handles + mappings, unchanged semantics
    uint64_t token;         // Milestone 30: opaque, process-independent lookup key -- see shm64_find_by_token
    struct shm64* dbg_next; // intrusive list of every live shm object, diagnostics only
} shm64_t;

// M+1A: allocates the physical backing via memobj64_create(size) (still
// physically contiguous, still zero-filled -- required since userspace
// must never observe another object's stale data), then kmalloc's this
// thin shm64_t view around it. refcount starts at 1 (the creating
// process's own handle -- see kernel/syscall64.c's sys64_shm_create).
// Returns 0 with *out set, or -1 (bad size, or physmem64 has no single
// contiguous run big enough) -- nothing is left allocated on failure.
// Behavior identical to pre-M+1A from every existing caller's view.
int shm64_create(uint64_t size, shm64_t** out);

// Milestone 30: shared-memory handles are per-process integers (an
// index into THAT process's own handle table -- include/handle64.h),
// meaningless to any other process. A compositor mapping a client's
// window surface is a genuinely different, unrelated, already-running
// process, so handle inheritance (parent->child at spawn, Milestone 26)
// cannot get it there. `token` is a monotonically-increasing, opaque
// 64-bit value assigned at creation (NOT a pointer or physical address
// -- see kernel/syscall64.c's SYS64_SHM_TOKEN/SYS64_SHM_OPEN_TOKEN) that
// the CREATING process can hand to another process over any channel it
// likes (e.g. a pipe message); that other process then calls
// SYS64_SHM_OPEN_TOKEN to obtain its OWN handle to the SAME object
// (bumping refcount exactly like spawn inheritance does). Returns NULL
// if no live object has that token.
shm64_t* shm64_find_by_token(uint64_t token);

// +1 reference -- called when mapping the object (kernel/uservm64.c)
// or when a handle referencing it is duplicated (spawn inheritance,
// kernel/process64.c).
void shm64_add_ref(shm64_t* s);

// -1 reference. Once it reaches zero, releases this view's one
// reference to its memobj64_t (freeing the physical backing, if that
// was the memobj's own last reference) and kfree's this shm64_t --
// safe to call this as the very last thing referencing the object anywhere, since by
// definition nothing else can still be mapping or holding it.
void shm64_release(shm64_t* s);

// Diagnostics: logs every live shared-memory object's page count/size
// and current refcount via klog(). Development use only, same
// precedent as pipe64_dump()/process64_dump().
void shm64_dump(void);

// Runs the Milestone 26 shared-memory self-test suite. Logs each case
// and a final tally via klog(). Returns 1 if every case passed.
int shm64_selftest(void);

#endif // SHM64_H
