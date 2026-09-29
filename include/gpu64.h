#ifndef GPU64_H
#define GPU64_H

#include <stdint.h>
#include "memobj64.h"

// M+1B: the generic GPU kernel core -- a resource manager, not a
// display server (Revision 2 §02). Sibling to kernel/blockdev64.c, not
// folded into display64: a GPU device does buffer-lifecycle work
// independent of any display output, and this milestone deliberately
// does not touch display64 at all (no display64_output_t array, no
// scanout/present/cursor operations -- those need real output/mode
// plumbing that doesn't exist yet and isn't part of M+1B's scope).
//
// Two different lifetime shapes on purpose, matching two already-
// established precedents in this codebase:
//   - gpu64_device_t: registered once at boot by a driver, lives for
//     the kernel's lifetime, no refcount -- exactly kernel/blockdev64.c's
//     blockdev64_t registry (unlocked: registration only ever happens
//     before any process is spawned, same as every existing driver).
//   - gpu64_buffer_t: created/destroyed at runtime by arbitrary
//     processes via syscalls, refcounted, token-shareable -- exactly
//     kernel/shm64.c's shm64_t (which itself is now a view over the
//     same memobj64_t layer this file's buffers also view -- see
//     include/memobj64.h). This is M+1A's second real consumer.
//
// Deliberately deferred, not built here (see this milestone's own
// report for the reasoning): gpu64_context_t (no per-process driver-
// resource-ID collision risk exists without a real multi-resource
// driver, and handle64/process64's existing close/inherit paths already
// give gpu64_buffer_t a cleanup anchor for free), SYS64_GPU_BUFFER_MAP
// (nothing in this milestone's required tests or the null driver needs
// a CPU mapping of the GPU-side view), generic command submission,
// fences, and any scanout/present/cursor operation (§04's own vtable
// keeps those slots NAMED for later, not implemented now).

typedef struct gpu64_device_s gpu64_device_t;
typedef struct gpu64_buffer_s gpu64_buffer_t;

typedef enum {
    GPU64_DEVICE_NULL = 1,    // the M+1B test/proof driver -- see kernel/gpu64.c
    GPU64_DEVICE_VIRTIO = 2,  // M+3: the real VirtIO-GPU 2D driver -- see kernel/virtio_gpu64.c
} gpu64_device_kind_t;

// M+7: the first real use of gpu64_device_t.capability_flags, named back
// in M+1B specifically for this ("e.g. CURSOR_PLANE, OVERLAY_PLANE").
// Diagnostic only -- set by kernel/virtio_gpu64.c when its hardware
// cursor backend successfully registers with display64
// (display64_cursor_available() remains the actual query path any code
// should use to decide whether hardware cursor is usable; this flag
// exists so gpu64_dump() and similar diagnostics can report it too).
#define GPU64_CAP_CURSOR_PLANE (1u << 0)

// Bitmask -- what a buffer may legitimately be used for. Phase 1 has no
// driver that actually enforces most of these (the null driver never
// touches real hardware), but the field exists now so a real driver
// later has somewhere to read intent from without an ABI change, and so
// gpu64_buffer_create() has something concrete to validate today
// (unknown bits are rejected -- see this file's own create() comment).
#define GPU64_USAGE_CPU_READ  (1u << 0)
#define GPU64_USAGE_CPU_WRITE (1u << 1)
#define GPU64_USAGE_SCANOUT   (1u << 2)
#define GPU64_USAGE_CURSOR    (1u << 3)
#define GPU64_USAGE_ALL_FLAGS (GPU64_USAGE_CPU_READ | GPU64_USAGE_CPU_WRITE | GPU64_USAGE_SCANOUT | GPU64_USAGE_CURSOR)

typedef struct {
    // Driver-specific setup for a buffer gpu64_buffer_create() has
    // ALREADY generically allocated and taken a memobj64 reference for
    // (backing/usage/owning_device are already filled in on `buf` by
    // the time this runs) -- e.g. assigning buf->driver_resource_id.
    // Returning -1 aborts creation: gpu64_buffer_create() releases the
    // memobj64 reference it took and frees `buf`, exactly like
    // shm64_create's own kmalloc-failure unwind. May be NULL if a
    // driver has no per-buffer setup to do (the null driver has none).
    int (*create_buffer)(gpu64_device_t* dev, gpu64_buffer_t* buf);
    // Driver-specific teardown, called by gpu64_buffer_release() right
    // BEFORE it drops its own memobj64 reference -- mirror image of
    // create_buffer. Never fails, matching shm64_release's own "safe to
    // call as the very last thing" contract. May be NULL.
    void (*destroy_buffer)(gpu64_device_t* dev, gpu64_buffer_t* buf);
} gpu64_device_ops_t;

struct gpu64_device_s {
    char name[16];                  // stable diagnostic identifier, e.g. "gpu-null0" -- see blockdev64_t's own field
    gpu64_device_kind_t kind;
    uint32_t capability_flags;      // GPU64_CAP_* bitmask -- diagnostic only, no syscall surface reads this directly (display64_cursor_available() is the real query path for cursor support)
    const gpu64_device_ops_t* ops;  // either vtable pointer may be NULL -- see gpu64_device_ops_t's own comments
    void* driver_data;              // opaque, driver-owned
    gpu64_device_t* next;           // registry linkage -- kernel/gpu64.c owns this field
};

// Registers `dev` (already fully filled in by the caller, including a
// unique `name`) in the global registry. `dev` must remain valid for
// the kernel's lifetime (drivers kmalloc it or use static storage --
// never a stack-local struct, same requirement as blockdev64_register).
// Returns 0, or -1 (duplicate name).
int gpu64_register(gpu64_device_t* dev);

// Looks up a previously registered device by name, or NULL.
gpu64_device_t* gpu64_find(const char* name);

// Registry iteration -- pass NULL to start, then each device returned
// in turn, until NULL. Mirrors blockdev64_iter() exactly.
gpu64_device_t* gpu64_iter(gpu64_device_t* prev);

// The phase-1 syscall surface identifies a device by a small integer
// (mirroring Revision 2's own SYS64_GPU_BUFFER_CREATE(device_idx, ...)
// signature) rather than requiring userspace to know a driver-specific
// name -- walks the registry list `idx` times (fine: phase 1 never has
// more than one device). Returns NULL if idx is out of range.
gpu64_device_t* gpu64_by_index(uint32_t idx);

int gpu64_count(void);

// Logs every registered device (name, kind) via klog(). Development use only.
void gpu64_dump(void);

// ── Buffers ──────────────────────────────────────────────────────────
// A thin, refcounted, token-shareable VIEW over a memobj64_t -- see
// include/memobj64.h and this file's own header comment for why this
// mirrors shm64_t's shape rather than owning any physical-backing state
// itself. Two independent gpu64_buffer_t/shm64_t views can legitimately
// point at the SAME memobj64_t at once (the whole point of M+1A): each
// holds exactly one memobj64 reference for its own lifetime, so
// destroying either one never frees physical memory the other still
// needs -- memobj64_release() only actually frees on the backing's own
// last reference, regardless of which VIEW kind that reference came
// from.
struct gpu64_buffer_s {
    memobj64_t* backing;             // this view's one, permanent reference -- see kernel/gpu64.c's gpu64_buffer_create/release
    uint32_t usage;                   // GPU64_USAGE_* bitmask, validated at creation
    uint32_t refcount;                 // THIS view's own refcount: handles only in phase 1 (no mapping concept yet -- see this file's header comment)
    uint64_t token;                    // opaque, process-independent lookup key -- identical mechanism to shm64_t's own token
    gpu64_device_t* owning_device;
    uint32_t driver_resource_id;       // opaque, driver-owned identity (e.g. a future VirtIO-GPU resource ID); 0 = none, never assigned by the generic core itself
    gpu64_buffer_t* dbg_next;          // intrusive list of every live buffer, diagnostics only
};

// Creates a buffer of at least `size` bytes backed by a FRESH
// memobj64_t (memobj64_create(size)), then lets `dev`'s create_buffer
// op (if any) do driver-specific setup. `usage` must be a subset of
// GPU64_USAGE_ALL_FLAGS (any other bit set is rejected). Returns 0 with
// *out set, or -1 (bad size, unknown usage bits, dev == NULL,
// memobj64_create failure, driver create_buffer refusal) -- nothing is
// left allocated on any failure path.
int gpu64_buffer_create(gpu64_device_t* dev, uint64_t size, uint32_t usage, gpu64_buffer_t** out);

// M+1B's actual proof of M+1A's design: wraps the SAME memobj64_t an
// existing view (typically a shm64_t) already references, taking its
// own independent reference via memobj64_add_ref() -- the caller
// retains whatever reference it already held on `backing`. Same usage
// validation and driver create_buffer hook as gpu64_buffer_create().
// Returns 0 with *out set, or -1.
int gpu64_buffer_wrap(gpu64_device_t* dev, memobj64_t* backing, uint32_t usage, gpu64_buffer_t** out);

// Looks up a live buffer by token, or NULL. Mirrors shm64_find_by_token
// exactly (same "0 is never a valid token" convention).
gpu64_buffer_t* gpu64_buffer_find_by_token(uint64_t token);

// +1 reference -- called when a handle referencing it is duplicated
// (spawn inheritance) or opened by token.
void gpu64_buffer_add_ref(gpu64_buffer_t* buf);

// -1 reference. Once it reaches zero: runs the driver's destroy_buffer
// hook (if any), releases this view's one memobj64 reference (freeing
// the physical backing only if that was the memobj's own last
// reference anywhere -- possibly not, if a shm64_t or another
// gpu64_buffer_t still holds one), and kfree's this gpu64_buffer_t.
// Calling this with refcount already 0 is a caller bug, logged and
// ignored rather than underflowing the counter or double-freeing --
// same discipline as memobj64_release's own guard.
void gpu64_buffer_release(gpu64_buffer_t* buf);

// Diagnostics: logs every live buffer's size/usage/refcount via klog().
void gpu64_dump_buffers(void);

// Runs the M+1B self-test suite (device registry, buffer lifecycle via
// a private null test driver, and the memobj64 second-consumer
// scenarios this milestone exists to prove). Logs each case and a
// final tally via klog(). Returns 1 if every case passed.
int gpu64_selftest(void);

#endif // GPU64_H
