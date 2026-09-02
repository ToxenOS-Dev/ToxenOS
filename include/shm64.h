#ifndef SHM64_H
#define SHM64_H

#include <stdint.h>

// Milestone 26: a real kernel shared-memory object, whose lifetime is
// independent of any single process's virtual mapping of it -- see
// kernel/uservm64.c's UVM64_REGION_SHM region kind for how a process
// actually maps one of these into its own address space, and
// include/handle64.h for how userspace refers to one (never a raw
// physical address, never this struct's pointer).
//
// Ownership model: `refcount` counts every outstanding REFERENCE,
// where a reference is held by EITHER an open per-process handle
// (kernel/process64.c's handle table -- one per HANDLE64_SHM entry) OR
// an active virtual mapping (kernel/uservm64.c's UVM64_REGION_SHM
// region -- one per uservm64_map_shm() call, dropped by the matching
// uservm64_munmap()). Both kinds increment/decrement the SAME counter
// uniformly. This is deliberately NOT "one reference per handle,
// implicitly covering however many times it's been mapped": a process
// can close its handle while keeping an existing mapping alive, or
// keep the handle open after unmapping -- exactly mirroring how a
// process can close a file's fd while an mmap of it stays valid.
// Physical pages are only returned to physmem64 (and this struct
// kfree'd) once refcount reaches zero, i.e. once every handle AND
// every mapping, in every process, is gone.
typedef struct shm64 {
    uint64_t base_phys;   // physmem64_alloc_pages(npages) -- one contiguous run
    uint32_t npages;
    uint32_t refcount;
    uint64_t token;        // Milestone 30: opaque, process-independent lookup key -- see shm64_find_by_token
    struct shm64* dbg_next; // intrusive list of every live shm object, diagnostics only
} shm64_t;

// Rounds `size` up to whole 4KB pages and allocates that many
// PHYSICALLY CONTIGUOUS pages via physmem64_alloc_pages(), which
// zero-fills them as part of its own existing guarantee -- required
// here since userspace must never observe another object's stale
// data. kmalloc's the tracking struct. refcount starts at 1 (the
// creating process's own handle -- see kernel/syscall64.c's
// sys64_shm_create). Returns 0 with *out set, or -1 (bad size, or
// physmem64 has no single contiguous run big enough) -- nothing is
// left allocated on failure.
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

// -1 reference. Once it reaches zero, frees every physical page back
// to physmem64 and kfree's the tracking struct -- safe to call this as
// the very last thing referencing the object anywhere, since by
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
