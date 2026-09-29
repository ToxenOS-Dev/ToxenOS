#ifndef MEMOBJ64_H
#define MEMOBJ64_H

#include <stdint.h>

// M+1A: the single owner of physical backing and its lifetime/refcount.
// Introduced as the layer BENEATH shm64 (which becomes a thin view over
// one of these -- see include/shm64.h's updated header comment) so a
// future gpu64_buffer can share the SAME physical backing as an
// existing shm64 surface without either one independently refcounting
// the same pages (see the GPU/Display architecture proposal's §05).
//
// Scope today (deliberately minimal, per the M+1A brief): every
// memobj64_t is backed by ONE physically contiguous run, exactly what
// shm64_t used to allocate directly via physmem64_alloc_pages(). Nothing
// above this layer is allowed to assume that forever, though --
// physical addresses are only ever obtained via memobj64_get_runs(),
// never a direct `->base_phys` read, so a future scatter-gather backing
// (MEMOBJ64_SCATTER_GATHER, not built yet) is an additive change to
// memobj64_get_runs()'s callers, not a rewrite of them.
typedef struct memobj64 {
    uint64_t base_phys;    // physmem64_alloc_pages(npages) -- one contiguous run, today's only kind
    uint32_t npages;
    uint32_t refcount;      // every reference to the PHYSICAL BACKING itself -- see shm64_t's own refcount for the separate, layered-above "view" refcount
    struct memobj64* dbg_next; // intrusive list of every live memobj, diagnostics only
} memobj64_t;

// One physically contiguous range within a memobj64's backing. Today
// memobj64_get_runs() always returns exactly one of these (this struct
// exists so callers are already shaped for more than one, once a real
// scatter-gather backing exists).
typedef struct {
    uint64_t phys;
    uint32_t npages;
} memobj64_run_t;

// Rounds `size` up to whole 4KB pages and allocates that many
// PHYSICALLY CONTIGUOUS pages via physmem64_alloc_pages() (zero-filled,
// same guarantee physmem64 has always provided). kmalloc's the tracking
// struct. refcount starts at 1 (the caller's own reference). Returns 0
// with *out set, or -1 (size == 0, size rounds up past the uint64_t
// range, or physmem64 has no single contiguous run big enough) --
// nothing is left allocated on failure.
int memobj64_create(uint64_t size, memobj64_t** out);

// +1 reference.
void memobj64_add_ref(memobj64_t* m);

// -1 reference. Once it reaches zero, frees every physical page back to
// physmem64 and kfree's the tracking struct. Calling this on an object
// whose refcount is already 0 (a double-release bug in the caller) is
// logged and ignored rather than underflowing the counter.
void memobj64_release(memobj64_t* m);

// Writes up to `max_runs` physical runs describing this object's entire
// backing into `runs_out`, and returns how many runs it actually has.
// Today this is always 1 (contiguous-only backing) -- callers must still
// size `runs_out`/`max_runs` for at least 1, and treat a return value
// greater than `max_runs` as "buffer too small, nothing written" (mirrors
// the shape a real scatter-gather backing will need later). Returns -1
// on that overflow case, or if `m` is NULL.
int memobj64_get_runs(memobj64_t* m, memobj64_run_t* runs_out, int max_runs);

// Diagnostics: logs every live memobj's page count/refcount via klog().
void memobj64_dump(void);

// Runs the M+1A memobj64 self-test suite. Logs each case and a final
// tally via klog(). Returns 1 if every case passed.
int memobj64_selftest(void);

#endif // MEMOBJ64_H
