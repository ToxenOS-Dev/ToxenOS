// kernel/virtio_gpu64.c — M+3: VirtIO-GPU 2D driver, C-side bridge.
// See include/virtio_gpu64.h for the full design rationale (why the
// wire protocol lives exclusively in Rust, why gpu64_buffer_create()'s
// generic API is deliberately NOT taught to infer image geometry, and
// why virtio_gpu64_bind_scanout() exists as a separate, explicit-
// geometry entry point instead).
#include <stdint.h>
#include "../include/virtio_gpu64.h"
#include "../include/virtio_pci64.h"
#include "../include/memobj64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/display64.h"
#include "../include/tsc64.h"
#include "../include/klog.h"
#include "../include/irq64.h"
#include "../include/virtio_irq64.h"
#include "../include/kmutex64.h"
#include "../include/process64.h"
#include "../include/timer64.h"

// M+3 item 15: same "no header exists for this function, declare it
// locally" precedent kernel/syscall64.c's own SYS64_GET_TICKS handler
// already established -- see that file's own comment.
extern uint64_t timer64_get_ticks(void);

static gpu64_device_t g_virtio_gpu_device;
static int g_virtio_gpu_registered = 0;

// M+4: the ONE persistent compositor-presentation buffer, or NULL if
// virtio_gpu64_init_compositor_backend() was never called or failed.
// Guarding virtio_gpu64_run_scanout_test()/the self-test's own scanout
// round-trip against this (below) prevents a debug-only code path from
// hijacking scanout 0 away from a live production compositor.
static gpu64_buffer_t* g_compositor_buf = 0;
static uint32_t g_compositor_width = 0, g_compositor_height = 0;

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

// M+3: the create_buffer hook intentionally does nothing beyond what
// the M+1B null driver already does -- a fresh gpu64_buffer_t has
// driver_resource_id == 0 (kmalloc'd/zeroed by gpu64_buffer_create's own
// wrap_existing_memobj, per include/gpu64.h), which this driver treats
// as "not yet bound to any VirtIO-GPU resource." Real resource creation
// only happens through virtio_gpu64_bind_scanout(), which requires real
// width/height the generic (size, usage)-only creation API has no way
// to supply. See this file's own header comment for the full reasoning.
static int virtio_gpu_create_buffer_hook(gpu64_device_t* dev, gpu64_buffer_t* buf) {
    (void)dev; (void)buf;
    return 0;
}

// Unconditionally safe to call on any buffer this driver owns, bound or
// not -- virtio_gpu64_unbind_scanout() itself is a no-op when
// driver_resource_id is already 0.
static void virtio_gpu_destroy_buffer_hook(gpu64_device_t* dev, gpu64_buffer_t* buf) {
    (void)dev;
    virtio_gpu64_unbind_scanout(buf);
}

static const gpu64_device_ops_t g_virtio_gpu_ops = {
    .create_buffer  = virtio_gpu_create_buffer_hook,
    .destroy_buffer = virtio_gpu_destroy_buffer_hook,
};

// ── M+11C: the ONE sleeping GPU command mutex (see include/kmutex64.h). Held by Rust's with_device()
// across a whole submit-to-retire command, INCLUDING the wait for the completion interrupt: contending
// processes block in the scheduler (never spin). Never taken from an interrupt handler.
static kmutex64_t g_gpu_ctl_mutex;
void toxenos_gpu_ctl_lock(void)   { kmutex64_lock(&g_gpu_ctl_mutex); }
void toxenos_gpu_ctl_unlock(void) { kmutex64_unlock(&g_gpu_ctl_mutex); }

static irq64_ret_t gpu_ctl_irq(void* ctx) { (void)ctx; return toxenos_virtio_gpu_ctl_irq() > 0 ? IRQ64_RET_HANDLED : IRQ64_RET_NONE; }
static irq64_ret_t gpu_cur_irq(void* ctx) { (void)ctx; return toxenos_virtio_gpu_cur_irq() > 0 ? IRQ64_RET_HANDLED : IRQ64_RET_NONE; }

static int gpu_begin(void* info) { return toxenos_virtio_gpu_init_begin((const virtio_pci64_transport_info_t*)info); }
static int gpu_queues(const uint32_t* vec, int n) { return toxenos_virtio_gpu_init_queues(vec[0], n > 1 ? vec[1] : 0xFFFFu); }
static int gpu_finish(void) { return toxenos_virtio_gpu_init_finish(); }
static void gpu_abort(void) { toxenos_virtio_gpu_init_abort(); }
static int gpu_poll_init(void* info) { return toxenos_virtio_gpu_init((const virtio_pci64_transport_info_t*)info); }

#if defined(VIRTIO_IRQ_FORCE_POLL)
#define GPU_FORCE_POLL 1
#else
#define GPU_FORCE_POLL 0
#endif
#ifdef VIRTIO_IRQ_FAIL_AT
#define GPU_FAIL_AT VIRTIO_IRQ_FAIL_AT
#else
#define GPU_FAIL_AT 0
#endif

int virtio_gpu64_init(void) {
    pci64_device_t* dev = pci64_find_device(PCI64_VENDOR_VIRTIO, PCI64_DEVICE_VIRTIO_GPU_MODERN, 0);
    if (!dev) {
        klog("virtio_gpu64: no modern VirtIO-GPU device found\n");
        return -1;
    }
    pci64_enable_device(dev);

    virtio_pci64_transport_info_t info;
    if (virtio_pci64_probe(dev, &info) < 0 || !info.ok) {
        klog("virtio_gpu64: device has no usable modern transport\n");
        return -1;
    }

    // M+11C: interrupt-driven setup (control queue + cursor queue on dedicated MSI-X entries; config vector
    // stays NO_VECTOR) with reset-and-retry POLL fallback -- see include/virtio_irq64.h.
    static const virtio_dev_ops_t gpu_ops = { gpu_begin, gpu_queues, gpu_finish, gpu_abort, gpu_poll_init, 0 };
    irq64_handler_t handlers[2] = { gpu_ctl_irq, gpu_cur_irq };
    const char* names[2] = { "virtio-gpu-ctl", "virtio-gpu-cursor" };
    int irc = virtio_irq_init_device(virtio_irq_dev(VIRTIO_DEV_GPU), VIRTIO_DEV_GPU, dev, &info, 2, handlers, names,
                                     &virtio_pci_real_ops, &gpu_ops, GPU_FORCE_POLL, GPU_FAIL_AT);
    if (irc < 0) {
        klog("virtio_gpu64: Rust driver init failed\n");
        return -1;
    }

    g_virtio_gpu_device.name[0]='v'; g_virtio_gpu_device.name[1]='g'; g_virtio_gpu_device.name[2]='p';
    g_virtio_gpu_device.name[3]='u'; g_virtio_gpu_device.name[4]='0'; g_virtio_gpu_device.name[5]=0;
    g_virtio_gpu_device.kind = GPU64_DEVICE_VIRTIO;
    g_virtio_gpu_device.capability_flags = 0;
    g_virtio_gpu_device.ops = &g_virtio_gpu_ops;
    g_virtio_gpu_device.driver_data = 0;

    if (gpu64_register(&g_virtio_gpu_device) < 0) {
        klog("virtio_gpu64: gpu64_register failed (duplicate name?)\n");
        return -1;
    }
    g_virtio_gpu_registered = 1;

    klog("virtio_gpu64: registered as a real gpu64_device_t ('vgpu0')\n");
    return 0;
}

int virtio_gpu64_get_display_info(virtio_gpu64_display_mode_t* out) {
    uint32_t flat[VIRTIO_GPU64_MAX_SCANOUTS * 5];
    int enabled = toxenos_virtio_gpu_get_display_info(flat);
    if (enabled < 0) return -1;
    for (int i = 0; i < VIRTIO_GPU64_MAX_SCANOUTS; i++) {
        out[i].x       = flat[i * 5 + 0];
        out[i].y       = flat[i * 5 + 1];
        out[i].width   = flat[i * 5 + 2];
        out[i].height  = flat[i * 5 + 3];
        out[i].enabled = flat[i * 5 + 4];
    }
    return enabled;
}

// Shared by virtio_gpu64_bind_scanout() and virtio_gpu64_run_scanout_test():
// builds the (phys, len-in-bytes) run array from `obj`'s memobj64_get_runs()
// and calls into Rust's attach_backing. `len` here is exact BYTE length
// (memobj64_run_t reports pages, not bytes -- converted and overflow-
// checked here, since a run's page count times 4096 could in principle
// exceed what a 32-bit VirtIO mem_entry.length field can express, even
// though no real allocation this milestone ever gets remotely close).
static int attach_memobj_backing(uint32_t resource_id, memobj64_t* obj) {
    memobj64_run_t runs[8];
    int n = memobj64_get_runs(obj, runs, 8);
    if (n < 1) {
        klog("virtio_gpu64: memobj64_get_runs failed or returned no runs\n");
        return -1;
    }

    uint64_t phys[8];
    uint32_t len[8];
    for (int i = 0; i < n; i++) {
        uint64_t bytes = (uint64_t)runs[i].npages * 0x1000ULL;
        if (bytes == 0 || bytes > 0xFFFFFFFFULL) {
            klog("virtio_gpu64: a memobj run's byte length does not fit a 32-bit VirtIO mem_entry -- rejected\n");
            return -1;
        }
        phys[i] = runs[i].phys;
        len[i] = (uint32_t)bytes;
    }

    return toxenos_virtio_gpu_attach_backing(resource_id, phys, len, (uint32_t)n);
}

int virtio_gpu64_bind_scanout(gpu64_buffer_t* buf, uint32_t width, uint32_t height) {
    if (!buf || buf->driver_resource_id != 0 || width == 0 || height == 0) return -1;

    uint32_t resource_id;
    if (toxenos_virtio_gpu_resource_create_2d(width, height, VIRTIO_GPU64_FORMAT_B8G8R8X8_UNORM, &resource_id) < 0) {
        return -1;
    }
    if (attach_memobj_backing(resource_id, buf->backing) < 0) {
        // Attach failed after create succeeded -- unref the orphaned
        // resource rather than leaking a host-side resource ID no
        // ToxenOS object will ever reference again.
        toxenos_virtio_gpu_resource_unref(resource_id);
        return -1;
    }

    buf->driver_resource_id = resource_id;
    return 0;
}

int virtio_gpu64_unbind_scanout(gpu64_buffer_t* buf) {
    if (!buf || buf->driver_resource_id == 0) return 0; // never bound -- nothing to do
    uint32_t resource_id = buf->driver_resource_id;
    // Spec-correct ordering: detach backing before unref, regardless of
    // whether detach itself reports success -- a resource this driver is
    // about to forget about must never be left referencing this buffer's
    // memobj backing from the host's point of view.
    toxenos_virtio_gpu_detach_backing(resource_id);
    toxenos_virtio_gpu_resource_unref(resource_id);
    buf->driver_resource_id = 0;
    return 0;
}

// ── M+4: real compositor backend ─────────────────────────────────────
// display64's generic GPU-backend flush hook (display64_gpu_flush_fn) --
// the ONLY function display64.c ever calls into this driver through
// (see include/display64.h's own header comment). Re-validates the
// rect against this driver's own idea of the resource's geometry even
// though display64.c already clips against the same numbers it was
// given at registration time -- crossing a module boundary is exactly
// where this milestone's own arithmetic-validation discipline applies,
// matching every other entry point in this file.
static int compositor_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_compositor_buf || g_compositor_buf->driver_resource_id == 0) return -1;
    uint32_t rid = g_compositor_buf->driver_resource_id;
    if (toxenos_virtio_gpu_transfer_to_host_2d(rid, x, y, w, h, g_compositor_width, g_compositor_height) < 0) return -1;
    if (toxenos_virtio_gpu_resource_flush(rid, x, y, w, h, g_compositor_width, g_compositor_height) < 0) return -1;
    return 0;
}

// M+9B: forward-declared -- registered from virtio_gpu64_init_compositor_backend()
// below, defined further down this same file (right after it, alongside
// the rest of the direct-scanout backend).
static int direct_bind_impl(memobj64_t* backing, uint32_t width, uint32_t height, void** out_handle);
static int direct_present_impl(void* handle, uint32_t width, uint32_t height,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h, int switch_active);
static int direct_leave_impl(void);
static void direct_unbind_impl(void* handle);

int virtio_gpu64_init_compositor_backend(void) {
    if (!g_virtio_gpu_registered) return -1;
    if (g_compositor_buf) {
        klog("virtio_gpu64: compositor backend already initialized -- skipping\n");
        return -1;
    }

    virtio_gpu64_display_mode_t modes[VIRTIO_GPU64_MAX_SCANOUTS];
    int enabled = virtio_gpu64_get_display_info(modes);
    if (enabled <= 0) {
        klog("virtio_gpu64: compositor backend: GET_DISPLAY_INFO reported no enabled scanout\n");
        return -1;
    }
    uint32_t width = 0, height = 0;
    for (int i = 0; i < VIRTIO_GPU64_MAX_SCANOUTS; i++) {
        if (modes[i].enabled) { width = modes[i].width; height = modes[i].height; break; }
    }
    if (width == 0 || height == 0) {
        klog("virtio_gpu64: compositor backend: reported scanout has zero geometry\n");
        return -1;
    }

    // Overflow-checked size computation -- same discipline as every
    // other arithmetic this milestone touches (§5).
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels == 0 || pixels > (0xFFFFFFFFFFFFFFFFULL / 4)) {
        klog("virtio_gpu64: compositor backend: geometry overflow\n");
        return -1;
    }
    uint64_t size_bytes = pixels * 4;

    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(&g_virtio_gpu_device, size_bytes, GPU64_USAGE_SCANOUT | GPU64_USAGE_CPU_WRITE, &buf) < 0) {
        klog("virtio_gpu64: compositor backend: gpu64_buffer_create failed\n");
        return -1;
    }

    // The "CPU view" this milestone's own brief asks to exercise
    // against the SAME memobj64 a gpu64_buffer_t wraps: here that view
    // is the KERNEL's own always-mapped virtual address for the
    // buffer's one physical run (physmem64_to_virt), not a second
    // independent userspace mmap -- display64's blit_row/put_pixel/
    // fill_rect/copy_rows all write directly through this pointer at
    // present time. No duplicate physical storage exists anywhere in
    // this path.
    memobj64_run_t runs[8];
    int n = memobj64_get_runs(buf->backing, runs, 8);
    if (n != 1) {
        klog("virtio_gpu64: compositor backend: unexpected run count for a fresh contiguous buffer\n");
        gpu64_buffer_release(buf);
        return -1;
    }
    uint32_t* px = (uint32_t*)physmem64_to_virt(runs[0].phys);

    // Known-safe initial frame -- SET_SCANOUT below must never point at
    // an uninitialized resource (this milestone's own item 6).
    for (uint64_t i = 0; i < pixels; i++) px[i] = 0x00000000u;

    if (virtio_gpu64_bind_scanout(buf, width, height) < 0) {
        klog("virtio_gpu64: compositor backend: bind_scanout failed\n");
        gpu64_buffer_release(buf);
        return -1;
    }

    if (toxenos_virtio_gpu_transfer_to_host_2d(buf->driver_resource_id, 0, 0, width, height, width, height) < 0) {
        klog("virtio_gpu64: compositor backend: initial transfer_to_host_2d failed\n");
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return -1;
    }

    if (toxenos_virtio_gpu_set_scanout(0, buf->driver_resource_id, 0, 0, width, height) < 0) {
        klog("virtio_gpu64: compositor backend: set_scanout failed\n");
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return -1;
    }

    if (toxenos_virtio_gpu_resource_flush(buf->driver_resource_id, 0, 0, width, height, width, height) < 0) {
        klog("virtio_gpu64: compositor backend: initial resource_flush failed\n");
        toxenos_virtio_gpu_set_scanout(0, 0, 0, 0, 1, 1);
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return -1;
    }

    // Publish for compositor_flush_rect() before registering with
    // display64 -- the moment display64_set_gpu_backend() returns
    // success, a present could theoretically arrive (kernel-only code
    // so far, but no reason to race the invariant).
    g_compositor_buf = buf;
    g_compositor_width = width;
    g_compositor_height = height;

    if (display64_set_gpu_backend((uint64_t)(uintptr_t)px, width, height, width * 4u, compositor_flush_rect) < 0) {
        klog("virtio_gpu64: compositor backend: display64_set_gpu_backend rejected -- tearing down\n");
        toxenos_virtio_gpu_set_scanout(0, 0, 0, 0, 1, 1);
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        g_compositor_buf = 0;
        g_compositor_width = 0;
        g_compositor_height = 0;
        return -1;
    }

    klog("virtio_gpu64: compositor backend live -- display64 now presenting through VirtIO-GPU\n");

    // M+9B: register the direct-scanout backend right alongside the
    // normal GPU presentation backend above -- not a hard requirement
    // (compositor64 tolerates DIRECT_SCANOUT_UNSUPPORTED fine), but
    // there is no reason for it to ever fail once the primary backend
    // itself is live, and no caller exists before this point that could
    // race it.
    if (display64_set_direct_scanout_backend(direct_bind_impl, direct_present_impl,
                                              direct_leave_impl, direct_unbind_impl) < 0) {
        klog("virtio_gpu64: direct-scanout backend registration failed -- M+9B stays unsupported this boot\n");
    }
    return 0;
}

// ── M+9B: direct-scanout / composition-bypass backend ────────────────
// See include/display64.h's own header comment for the exact contract
// each of these four must satisfy. All reuse the SAME proven primitives
// (gpu64_buffer_wrap, virtio_gpu64_bind_scanout/unbind_scanout, the raw
// toxenos_virtio_gpu_* commands) the compositor's own g_compositor_buf
// above already exercises -- no new VirtIO command sequence, no new
// resource-lifetime mechanism.
static int direct_bind_impl(memobj64_t* backing, uint32_t width, uint32_t height, void** out_handle) {
    if (display64_direct_fault_should_fail(M9B_FAULT_BIND)) return -1;

    // gpu64_buffer_wrap() takes its OWN independent memobj64 reference
    // on `backing` (see include/gpu64.h's own header comment) -- this
    // is the ENTIRE "client buffer -> GPU resource" step this
    // milestone's own §1/§4 asked to find the smallest clean API for;
    // nothing here allocates a second copy of the client's pixels.
    gpu64_buffer_t* buf;
    if (gpu64_buffer_wrap(&g_virtio_gpu_device, backing, GPU64_USAGE_SCANOUT, &buf) < 0) return -1;

    if (virtio_gpu64_bind_scanout(buf, width, height) < 0) {
        gpu64_buffer_release(buf); // drops the memobj64 reference just taken -- nothing leaked
        return -1;
    }

    *out_handle = (void*)buf;
    return 0;
}

// M+10 fault-injection campaign finding: the ORIGINAL version of this
// function returned a plain int, and atomic_commit_impl() classified
// FAIL_CLEAN vs FAIL_PARTIAL purely by "did an EARLIER delta group
// already succeed" -- wrong, because TRANSFER succeeding before a LATER
// SET_SCANOUT/RESOURCE_FLUSH failure IN THIS SAME CALL is a genuine
// hardware mutation (VirtIO-GPU's TRANSFER_TO_HOST_2D writes pixel data
// into the resource's host-side backing store immediately, regardless
// of whether SET_SCANOUT/RESOURCE_FLUSH ever make it visible), yet the
// old classification reported this as CLEAN if this happened to be the
// FIRST delta group processed. Fixed by having this function itself
// report exactly how far it got, at VirtIO-command granularity -- the
// only place that actually knows.
#ifdef M9B_FAULT_INJECT
#define M9B_TRACE_CMD(name) klog("M9B_FAULT_INJECT: issuing " name "\n")
#else
#define M9B_TRACE_CMD(name) do { } while (0)
#endif
static display64_commit_result_t direct_present_impl_g(void* handle, uint32_t width, uint32_t height,
                                                         uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                                         int switch_active)
{
    gpu64_buffer_t* buf = (gpu64_buffer_t*)handle;
    if (!buf || buf->driver_resource_id == 0) return DISPLAY64_COMMIT_FAIL_CLEAN; // nothing attempted yet
    uint32_t rid = buf->driver_resource_id;

    // Exact ordering per this milestone's own Linux reference audit
    // (virtio_gpu_primary_plane_update): TRANSFER_TO_HOST_2D for the
    // damaged rect first (this is a classic CPU-backed 2D resource, no
    // guest_blob/host3d_blob path exists in this driver), SET_SCANOUT
    // ONLY if the active resource is actually changing -- never
    // unconditionally every frame -- then RESOURCE_FLUSH always.
    M9B_TRACE_CMD("TRANSFER_TO_HOST_2D (primary)");
    if (display64_direct_fault_should_fail(M9B_FAULT_TRANSFER)) return DISPLAY64_COMMIT_FAIL_CLEAN; // first command -- nothing issued yet
    if (toxenos_virtio_gpu_transfer_to_host_2d(rid, x, y, w, h, width, height) < 0) return DISPLAY64_COMMIT_FAIL_CLEAN;

    if (switch_active) {
        M9B_TRACE_CMD("SET_SCANOUT (primary)");
        if (display64_direct_fault_should_fail(M9B_FAULT_SET_SCANOUT)) return DISPLAY64_COMMIT_FAIL_PARTIAL; // TRANSFER already landed -- real mutation occurred
        if (toxenos_virtio_gpu_set_scanout(0, rid, 0, 0, width, height) < 0) return DISPLAY64_COMMIT_FAIL_PARTIAL;
    }

    M9B_TRACE_CMD("RESOURCE_FLUSH (primary)");
    if (display64_direct_fault_should_fail(M9B_FAULT_RESOURCE_FLUSH)) return DISPLAY64_COMMIT_FAIL_PARTIAL; // TRANSFER (and maybe SET_SCANOUT) already landed
    if (toxenos_virtio_gpu_resource_flush(rid, x, y, w, h, width, height) < 0) return DISPLAY64_COMMIT_FAIL_PARTIAL;

    return DISPLAY64_COMMIT_OK;
}

// Compatibility shim for display64_set_direct_scanout_backend()'s own
// int-returning registration typedef -- no longer actually invoked
// through that function pointer (atomic_commit_impl calls
// direct_present_impl_g() directly for its fine-grained result); kept
// only so the registration/capability-check call site still compiles
// and null-checks correctly.
static int direct_present_impl(void* handle, uint32_t width, uint32_t height,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                int switch_active)
{
    return direct_present_impl_g(handle, width, height, x, y, w, h, switch_active) == DISPLAY64_COMMIT_OK ? 0 : -1;
}

// Switches scanout 0 back to g_compositor_buf's own already-bound,
// already-content-fresh resource (compositor64 is required to have
// already recomposited and run its own NORMAL present path, which
// TRANSFER_TO_HOST_2D's g_compositor_buf's resource, before ever
// calling this -- see this milestone's own §11 ordering) and flushes
// it -- no TRANSFER here, matching the Linux reference's own "always
// flush, only transfer/set_scanout when something actually changed"
// pattern (content freshness is the normal present path's job; this
// function's only job is the scanout switch-back + confirming flush).
// Same fine-grained-result fix as direct_present_impl_g() above.
static display64_commit_result_t direct_leave_impl_g(void) {
    if (!g_compositor_buf || g_compositor_buf->driver_resource_id == 0) return DISPLAY64_COMMIT_FAIL_CLEAN;
    uint32_t rid = g_compositor_buf->driver_resource_id;

    M9B_TRACE_CMD("SET_SCANOUT (leave -> compositor)");
    if (display64_direct_fault_should_fail(M9B_FAULT_SET_SCANOUT)) return DISPLAY64_COMMIT_FAIL_CLEAN; // first command -- nothing issued yet
    if (toxenos_virtio_gpu_set_scanout(0, rid, 0, 0, g_compositor_width, g_compositor_height) < 0) return DISPLAY64_COMMIT_FAIL_CLEAN;

    M9B_TRACE_CMD("RESOURCE_FLUSH (leave -> compositor)");
    if (display64_direct_fault_should_fail(M9B_FAULT_RESOURCE_FLUSH)) return DISPLAY64_COMMIT_FAIL_PARTIAL; // SET_SCANOUT already switched hardware -- a real, visible mutation
    if (toxenos_virtio_gpu_resource_flush(rid, 0, 0, g_compositor_width, g_compositor_height,
                                          g_compositor_width, g_compositor_height) < 0) return DISPLAY64_COMMIT_FAIL_PARTIAL;

    return DISPLAY64_COMMIT_OK;
}

static int direct_leave_impl(void) {
    return direct_leave_impl_g() == DISPLAY64_COMMIT_OK ? 0 : -1;
}

// gpu64_buffer_release()'s own destroy_buffer hook (virtio_gpu_destroy_buffer_hook,
// defined earlier in this file) already calls virtio_gpu64_unbind_scanout()
// -- DETACH_BACKING + RESOURCE_UNREF -- so this is the entire teardown:
// one call correctly tears down both the VirtIO resource and the
// memobj64 reference direct_bind_impl() took, in the right order,
// exactly like every other gpu64_buffer_t this driver ever releases.
static void direct_unbind_impl(void* handle) {
    gpu64_buffer_t* buf = (gpu64_buffer_t*)handle;
    if (!buf) return;
    gpu64_buffer_release(buf);
}

// ── M+7: hardware cursor ──────────────────────────────────────────────
// A separate, independent resource/backend from the compositor's own
// primary scanout buffer above -- different resource ID, different
// virtqueue (cursor, never control), no shared state at all. This is
// exactly why the M+3 debug/self-test paths' existing "refuse if a
// compositor backend is already live" guard needs no cursor-specific
// counterpart: those paths only ever touch scanout 0's PRIMARY resource
// via SET_SCANOUT/the control queue, never the cursor queue or this
// resource, so there is no shared state for them to corrupt in the
// first place.
static gpu64_buffer_t* g_cursor_buf = 0;
static uint32_t* g_cursor_px = 0;      // kernel-mapped CPU view of the cursor resource's own backing
static int g_cursor_ready = 0;          // 1 once queue+resource+backend registration all succeeded
static int32_t g_cursor_last_x = 0, g_cursor_last_y = 0; // last position given to move/update -- remembered across a hide/show cycle
static int g_cursor_visible = 1;        // logical visibility -- see cursor_backend_set_visible()

// display64_cursor_set_image_fn: uploads new cursor pixel data (already
// copied into kernel memory by the SYS64_CURSOR_SET_IMAGE handler) and
// establishes it as the cursor image via UPDATE_CURSOR. Rejects any size
// other than the fixed VIRTIO_GPU64_CURSOR_DIM this driver's own
// resource was created at -- there is exactly one persistent cursor
// resource for this boot session, never recreated per call (this
// milestone's own explicit requirement).
// Same fine-grained-result fix as direct_present_impl_g() -- the CPU-
// side pixel copy above is a plain kernel-memory write, not a hardware
// mutation; TRANSFER_TO_HOST_2D is the first command that actually
// touches the device, so a failure before it is CLEAN, a failure at
// UPDATE_CURSOR after a successful TRANSFER is PARTIAL (the resource's
// host-side backing store already has the new pixels, even though the
// on-screen cursor hasn't been told to redisplay them yet).
static display64_commit_result_t cursor_backend_set_image_g(uint32_t width, uint32_t height, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y) {
    if (!g_cursor_ready || !g_cursor_buf) return DISPLAY64_COMMIT_FAIL_CLEAN;
    if (width != VIRTIO_GPU64_CURSOR_DIM || height != VIRTIO_GPU64_CURSOR_DIM) return DISPLAY64_COMMIT_FAIL_CLEAN;

    uint64_t n = (uint64_t)width * height;
    for (uint64_t i = 0; i < n; i++) g_cursor_px[i] = argb_pixels[i];

    uint32_t rid = g_cursor_buf->driver_resource_id;
    M9B_TRACE_CMD("TRANSFER_TO_HOST_2D (cursor image)");
    if (display64_direct_fault_should_fail(M9B_FAULT_CURSOR_TRANSFER)) return DISPLAY64_COMMIT_FAIL_CLEAN; // first hardware command -- nothing issued yet
    if (toxenos_virtio_gpu_transfer_to_host_2d(rid, 0, 0, width, height, width, height) < 0) return DISPLAY64_COMMIT_FAIL_CLEAN;
    if (!g_cursor_visible) return DISPLAY64_COMMIT_OK; // image updated for later; an explicit hide is never silently overridden by a new image upload
    uint32_t ux = g_cursor_last_x < 0 ? 0 : (uint32_t)g_cursor_last_x;
    uint32_t uy = g_cursor_last_y < 0 ? 0 : (uint32_t)g_cursor_last_y;
    M9B_TRACE_CMD("UPDATE_CURSOR (cursor image)");
    if (display64_direct_fault_should_fail(M9B_FAULT_CURSOR_UPDATE)) return DISPLAY64_COMMIT_FAIL_PARTIAL; // TRANSFER already landed -- real mutation occurred
    if (toxenos_virtio_gpu_cursor_update(0, rid, ux, uy, hot_x, hot_y) < 0) return DISPLAY64_COMMIT_FAIL_PARTIAL;
    return DISPLAY64_COMMIT_OK;
}

static int cursor_backend_set_image(uint32_t width, uint32_t height, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y) {
    return cursor_backend_set_image_g(width, height, argb_pixels, hot_x, hot_y) == DISPLAY64_COMMIT_OK ? 0 : -1;
}

// display64_cursor_move_fn: the cheap, common-case operation -- just
// MOVE_CURSOR, never a resource re-upload. Clamps a negative logical
// position to 0 (compositor64's own input handling already clamps
// g_cursor_x/y to the display bounds before ever calling this, but this
// driver doesn't assume a well-behaved caller across the syscall
// boundary either).
static int cursor_backend_move(int32_t x, int32_t y) {
    if (!g_cursor_ready) return -1;
    g_cursor_last_x = x; g_cursor_last_y = y;
    if (!g_cursor_visible) return 0; // nothing to move on-device while hidden -- position is remembered for when shown again
    uint32_t ux = x < 0 ? 0 : (uint32_t)x;
    uint32_t uy = y < 0 ? 0 : (uint32_t)y;
    M9B_TRACE_CMD("MOVE_CURSOR");
    if (display64_direct_fault_should_fail(M9B_FAULT_CURSOR_MOVE)) return -1; // the only command this function ever issues -- a failure here is always CLEAN by construction
    if (toxenos_virtio_gpu_cursor_move(0, g_cursor_buf->driver_resource_id, ux, uy) < 0) return -1;
    return 0;
}

// display64_cursor_set_visible_fn: UPDATE_CURSOR with resource_id == 0
// hides the cursor per spec; passing the real resource_id again (at the
// last remembered position) shows it again without any re-upload.
static int cursor_backend_set_visible(int visible) {
    if (!g_cursor_ready) return -1;
    g_cursor_visible = visible ? 1 : 0;
    uint32_t rid = g_cursor_visible ? g_cursor_buf->driver_resource_id : 0;
    uint32_t ux = g_cursor_last_x < 0 ? 0 : (uint32_t)g_cursor_last_x;
    uint32_t uy = g_cursor_last_y < 0 ? 0 : (uint32_t)g_cursor_last_y;
    if (toxenos_virtio_gpu_cursor_update(0, rid, ux, uy, VIRTIO_GPU64_CURSOR_HOT_X, VIRTIO_GPU64_CURSOR_HOT_Y) < 0) return -1;
    return 0;
}

int virtio_gpu64_cursor_available(void) { return g_cursor_ready; }

// ── M+10: atomic backend (check/commit) ──────────────────────────────
// display64.c no longer calls direct_present_impl/direct_leave_impl/
// cursor_backend_move/set_visible/set_image directly as four
// independent, immediately-applied operations -- it now computes a
// delta against its own authoritative current state and calls exactly
// ONE of these two functions, which translate that delta into the SAME
// underlying command sequences (unchanged, already-proven from M+7/M+9B)
// in the order §9 requires: primary before cursor, image before
// position/visibility. See docs/m10_current_mutation_audit.md for why
// reusing these exact functions (rather than rewriting their VirtIO
// command sequences from scratch) was the deliberate, lower-risk choice.
static display64_check_result_t atomic_check_impl(const display64_state_t* old_state, const display64_state_t* new_state, display64_delta_t delta) {
    (void)old_state;
    if (delta & DISPLAY64_DELTA_CURSOR_IMAGE) {
        if (new_state->cursor.img_width != VIRTIO_GPU64_CURSOR_DIM || new_state->cursor.img_height != VIRTIO_GPU64_CURSOR_DIM) {
            return DISPLAY64_CHECK_BAD_CURSOR;
        }
        if (!g_cursor_ready) return DISPLAY64_CHECK_NO_BACKEND;
    }
    if (delta & (DISPLAY64_DELTA_CURSOR_POSITION | DISPLAY64_DELTA_CURSOR_VISIBILITY)) {
        if (!g_cursor_ready) return DISPLAY64_CHECK_NO_BACKEND;
    }
    if ((delta & DISPLAY64_DELTA_PRIMARY_CHANGED) && new_state->primary.kind == DISPLAY64_PRIMARY_DIRECT) {
        if (!new_state->primary.backend_handle) return DISPLAY64_CHECK_BAD_PRIMARY;
    }
    return DISPLAY64_CHECK_OK;
}

static display64_commit_result_t atomic_commit_impl(const display64_state_t* old_state, const display64_state_t* new_state, display64_delta_t delta) {
    (void)old_state;
    int any_issued = 0;

    // §9: primary before cursor -- a resource/content change on the
    // primary never depends on cursor state, but a cursor move/image
    // upload against a primary that's mid-transition would be a
    // meaningless ordering to reason about, so primary always goes
    // first, matching M+9B's own already-proven ordering.
    //
    // Uses the fine-grained _g() variants (not the plain-int compat
    // shims) so a failure PARTWAY THROUGH one of these calls -- e.g.
    // TRANSFER succeeding before a later SET_SCANOUT/RESOURCE_FLUSH
    // fails -- is classified as PARTIAL on its own merits, never
    // downgraded to CLEAN just because this happens to be the very
    // FIRST delta group this function ever attempts in a commit (a real
    // gap the M+10 fault-injection campaign found and this fixes).
    if (delta & (DISPLAY64_DELTA_PRIMARY_CHANGED | DISPLAY64_DELTA_PRIMARY_CONTENT)) {
        display64_commit_result_t prc;
        if (new_state->primary.kind == DISPLAY64_PRIMARY_DIRECT) {
            int switch_active = (delta & DISPLAY64_DELTA_PRIMARY_CHANGED) ? 1 : 0;
            uint32_t dx = new_state->primary.has_damage ? new_state->primary.damage_x : 0;
            uint32_t dy = new_state->primary.has_damage ? new_state->primary.damage_y : 0;
            uint32_t dw = new_state->primary.has_damage ? new_state->primary.damage_w : new_state->primary.width;
            uint32_t dh = new_state->primary.has_damage ? new_state->primary.damage_h : new_state->primary.height;
            prc = direct_present_impl_g(new_state->primary.backend_handle, new_state->primary.width, new_state->primary.height, dx, dy, dw, dh, switch_active);
        } else {
            // PRIMARY_CHANGED with the new kind == COMPOSITED: leaving
            // direct mode. old_state was DIRECT (compute_delta() only
            // sets this bit for a genuine kind/identity difference) --
            // direct_leave_impl_g()'s own precondition (the compositor's
            // own resource already fresh via the normal present path)
            // is unchanged from M+9B's own §11 ordering.
            prc = direct_leave_impl_g();
        }
        if (prc != DISPLAY64_COMMIT_OK) return (any_issued && prc == DISPLAY64_COMMIT_FAIL_CLEAN) ? DISPLAY64_COMMIT_FAIL_PARTIAL : prc;
        any_issued = 1;
    }

    if (delta & DISPLAY64_DELTA_CURSOR_IMAGE) {
        display64_commit_result_t prc = cursor_backend_set_image_g(new_state->cursor.img_width, new_state->cursor.img_height,
                                                                      new_state->cursor.argb_pixels, new_state->cursor.hot_x, new_state->cursor.hot_y);
        if (prc != DISPLAY64_COMMIT_OK) return (any_issued && prc == DISPLAY64_COMMIT_FAIL_CLEAN) ? DISPLAY64_COMMIT_FAIL_PARTIAL : prc;
        any_issued = 1;
    }
    if (delta & DISPLAY64_DELTA_CURSOR_VISIBILITY) {
        int rc = cursor_backend_set_visible(new_state->cursor.enabled);
        if (rc < 0) return any_issued ? DISPLAY64_COMMIT_FAIL_PARTIAL : DISPLAY64_COMMIT_FAIL_CLEAN;
        any_issued = 1;
    }
    if (delta & DISPLAY64_DELTA_CURSOR_POSITION) {
        int rc = cursor_backend_move(new_state->cursor.x, new_state->cursor.y);
        if (rc < 0) return any_issued ? DISPLAY64_COMMIT_FAIL_PARTIAL : DISPLAY64_COMMIT_FAIL_CLEAN;
        any_issued = 1;
    }

    return DISPLAY64_COMMIT_OK;
}

// Registers the atomic backend and runs the M+10 self-tests once, right
// here -- guaranteed to run before any real client exists (this is
// called from kernel64.c immediately after cursor init, still during
// early boot), so display64_atomic_selftest()'s own synthetic identity
// can never collide with a real one. Returns 0 on successful
// registration (regardless of self-test pass/fail -- a self-test
// failure is logged, not fatal to boot, matching every other
// *_selftest() call site's own convention in this codebase), or -1 if
// display64_set_atomic_backend() itself was rejected.
// ── M+10 fault-injection campaign (mandatory before acceptance) ──────
// Lives here, not in display64.c, specifically because it needs
// backend-private knowledge (VIRTIO_GPU64_CURSOR_DIM, the exact command
// sequences) to construct scenarios that reach past display64_state_check()
// -- display64.c itself must stay VirtIO-agnostic (see this file's own
// top comment), so this test speaks only through the generic
// display64_state_*() API plus the fault-injection arm functions, never
// touching g_cursor_buf/g_compositor_buf internals directly except to
// size the synthetic test image correctly.
//
// Only meaningfully runs when M9B_FAULT_INJECT is defined -- a plain
// boot has no fault points to arm, so every fault-dependent case would
// trivially "pass" by never seeing a failure at all, which would be
// dishonest to report as a real test. Guarded out entirely otherwise
// (matching virtio_gpu64_selftest()'s own "absence is a first-class
// outcome" precedent).
#ifdef M9B_FAULT_INJECT
#define M10FT_IDENTITY 0xFFFFFFFFFFFFFFFDULL

static int m10ft_check(int* pass, int* fail, const char* name, int ok) {
    klog("display64_atomic_fault_selftest: "); klog(name); klog(ok ? " PASS\n" : " FAIL\n");
    if (ok) { (*pass)++; return 1; }
    (*fail)++;
    return 0;
}

int display64_atomic_fault_selftest(void) {
    int pass = 0, fail = 0;

    if (!g_virtio_gpu_registered || !g_compositor_buf) {
        klog("display64_atomic_fault_selftest: no live VirtIO-GPU compositor backend -- SKIPPED\n");
        return 1;
    }

    uint32_t w = g_compositor_width, h = g_compositor_height;
    memobj64_t* test_obj = 0;
    if (memobj64_create((uint64_t)w * h * 4u, &test_obj) < 0) {
        return m10ft_check(&pass, &fail, "setup: test memobj64 allocated", 0);
    }
    void* out_handle = 0;
    if (display64_direct_scanout_bind(M10FT_IDENTITY, test_obj, w, h, &out_handle) < 0) {
        m10ft_check(&pass, &fail, "setup: test identity bound", 0);
        return fail == 0;
    }
    m10ft_check(&pass, &fail, "setup: test identity bound", 1);

    display64_state_t before;
    int ok;

    // ── §1a: TRANSFER fault entering DIRECT -- must be CLEAN ──────────
    display64_direct_fault_inject_clear_all();
    display64_state_get_current(&before);
    display64_direct_fault_inject_arm(M9B_FAULT_TRANSFER);
    int rc = display64_direct_present(M10FT_IDENTITY, 0, 0, w, h, 1);
    display64_state_t after; display64_state_get_current(&after);
    ok = (rc < 0)
      && after.primary.kind == before.primary.kind
      && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_CLEAN
      && display64_state_get_validity() == DISPLAY64_STATE_VALID;
    m10ft_check(&pass, &fail, "1a: TRANSFER fault entering DIRECT -> CLEAN, current unchanged", ok);

    // ── §1b: SET_SCANOUT fault entering DIRECT -- must be PARTIAL, recovery succeeds -> VALID ──
    display64_direct_fault_inject_clear_all();
    uint64_t rec_attempts_before = display64_state_get_recovery_attempts();
    uint64_t rec_successes_before = display64_state_get_recovery_successes();
    display64_direct_fault_inject_arm(M9B_FAULT_SET_SCANOUT);
    rc = display64_direct_present(M10FT_IDENTITY, 0, 0, w, h, 1);
    display64_state_get_current(&after);
    ok = (rc < 0)
      && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_PARTIAL
      && display64_state_get_recovery_attempts() == rec_attempts_before + 1
      && display64_state_get_recovery_successes() == rec_successes_before + 1
      && display64_state_get_validity() == DISPLAY64_STATE_VALID
      && after.primary.kind == DISPLAY64_PRIMARY_COMPOSITED; // recovery's own target
    m10ft_check(&pass, &fail, "1b: SET_SCANOUT fault (TRANSFER already landed) -> PARTIAL, rollback succeeds -> VALID/composited", ok);

    // ── §1c: RESOURCE_FLUSH fault entering DIRECT -- must be PARTIAL, recovery succeeds ──
    display64_direct_fault_inject_clear_all();
    rec_attempts_before = display64_state_get_recovery_attempts();
    rec_successes_before = display64_state_get_recovery_successes();
    display64_direct_fault_inject_arm(M9B_FAULT_RESOURCE_FLUSH);
    rc = display64_direct_present(M10FT_IDENTITY, 0, 0, w, h, 1);
    display64_state_get_current(&after);
    ok = (rc < 0)
      && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_PARTIAL
      && display64_state_get_recovery_attempts() == rec_attempts_before + 1
      && display64_state_get_recovery_successes() == rec_successes_before + 1
      && display64_state_get_validity() == DISPLAY64_STATE_VALID
      && after.primary.kind == DISPLAY64_PRIMARY_COMPOSITED;
    m10ft_check(&pass, &fail, "1c: RESOURCE_FLUSH fault (TRANSFER+SET_SCANOUT already landed) -> PARTIAL, rollback succeeds", ok);

    // ── §1d/e/f: cursor faults ─────────────────────────────────────────
    uint32_t* cursor_px = (uint32_t*)kmalloc((uint64_t)VIRTIO_GPU64_CURSOR_DIM * VIRTIO_GPU64_CURSOR_DIM * 4u);
    if (cursor_px) {
        for (uint32_t i = 0; i < (uint32_t)VIRTIO_GPU64_CURSOR_DIM * VIRTIO_GPU64_CURSOR_DIM; i++) cursor_px[i] = 0x00AABBCCu;

        display64_direct_fault_inject_clear_all();
        display64_state_get_current(&before);
        display64_direct_fault_inject_arm(M9B_FAULT_CURSOR_TRANSFER);
        rc = display64_cursor_set_image(VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM, cursor_px, 0, 0);
        display64_state_get_current(&after);
        ok = (rc < 0)
          && after.cursor.image_generation == before.cursor.image_generation
          && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_CLEAN
          && display64_state_get_validity() == DISPLAY64_STATE_VALID;
        m10ft_check(&pass, &fail, "1d: cursor TRANSFER fault -> CLEAN, cursor image unchanged", ok);

        display64_direct_fault_inject_clear_all();
        rec_attempts_before = display64_state_get_recovery_attempts();
        rec_successes_before = display64_state_get_recovery_successes();
        display64_direct_fault_inject_arm(M9B_FAULT_CURSOR_UPDATE);
        rc = display64_cursor_set_image(VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM, cursor_px, 0, 0);
        ok = (rc < 0)
          && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_PARTIAL
          && display64_state_get_recovery_attempts() == rec_attempts_before + 1
          && display64_state_get_recovery_successes() == rec_successes_before + 1
          && display64_state_get_validity() == DISPLAY64_STATE_VALID;
        m10ft_check(&pass, &fail, "1e: cursor UPDATE_CURSOR fault (TRANSFER already landed) -> PARTIAL, rollback succeeds", ok);

        display64_direct_fault_inject_clear_all();
        display64_state_get_current(&before);
        display64_direct_fault_inject_arm(M9B_FAULT_CURSOR_MOVE);
        rc = display64_cursor_move(5, 5);
        display64_state_get_current(&after);
        ok = (rc < 0)
          && after.cursor.x == before.cursor.x && after.cursor.y == before.cursor.y
          && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_CLEAN;
        m10ft_check(&pass, &fail, "1f: cursor MOVE_CURSOR fault (single command) -> CLEAN, position unchanged", ok);

        kfree(cursor_px);
    } else {
        m10ft_check(&pass, &fail, "1d/1e/1f: cursor fault tests (kmalloc for test image)", 0);
    }

    // ── §3/§4: partial failure whose OWN rollback also fails -> RECOVERY_REQUIRED, then a later successful recovery -> VALID ──
    display64_direct_fault_inject_clear_all();
    // First, get cleanly into DIRECT mode (no fault armed) as the "old known-good state" the failed leave will try to preserve/restore.
    rc = display64_direct_present(M10FT_IDENTITY, 0, 0, w, h, 1);
    ok = (rc == 0) && display64_state_get_validity() == DISPLAY64_STATE_VALID;
    m10ft_check(&pass, &fail, "3-prep: clean entry into DIRECT before rollback-failure test", ok);

    display64_state_get_current(&before); // DIRECT, primary.identity == M10FT_IDENTITY
    rec_attempts_before = display64_state_get_recovery_attempts();
    // Arm BOTH: the leave's own RESOURCE_FLUSH (after its SET_SCANOUT
    // already succeeded -- a genuine PARTIAL) AND the recovery's own
    // rollback point, so recovery -- triggered automatically inside
    // this same display64_state_commit() call -- ALSO fails.
    display64_direct_fault_inject_arm(M9B_FAULT_RESOURCE_FLUSH);
    display64_direct_fault_inject_arm(M9B_FAULT_ROLLBACK);
    rc = display64_leave_direct_scanout();
    display64_state_get_current(&after);
    ok = (rc < 0)
      && display64_state_get_last_commit_result() == DISPLAY64_COMMIT_FAIL_PARTIAL
      && display64_state_get_recovery_attempts() == rec_attempts_before + 1
      && display64_state_get_validity() == DISPLAY64_STATE_RECOVERY_REQUIRED
      // §11: current must be left COMPLETELY UNTOUCHED -- neither the
      // failed leave's candidate (composited) NOR any half-state was
      // adopted; the resource this window is still (possibly) scanned
      // out from must remain exactly as it was, still referenced.
      && after.primary.kind == before.primary.kind
      && after.primary.identity == before.primary.identity;
    m10ft_check(&pass, &fail, "4: leave fails PARTIAL + rollback also fails -> RECOVERY_REQUIRED, current left untouched", ok);

    // While RECOVERY_REQUIRED, every ordinary transaction must be refused.
    display64_state_t blocked;
    display64_state_begin(&blocked);
    display64_check_result_t brc = display64_state_check(&blocked);
    display64_state_discard(&blocked);
    m10ft_check(&pass, &fail, "4b: ordinary commit refused while RECOVERY_REQUIRED", brc == DISPLAY64_CHECK_RECOVERY_REQUIRED);

    // Now the mandatory next step: a subsequent successful recovery attempt -> VALID.
    display64_direct_fault_inject_clear_all(); // no fault armed this time -- recovery's own commands must be allowed to actually succeed
    int retry_rc = display64_state_retry_recovery();
    display64_state_get_current(&after);
    ok = (retry_rc == 0)
      && display64_state_get_validity() == DISPLAY64_STATE_VALID
      && after.primary.kind == DISPLAY64_PRIMARY_COMPOSITED;
    m10ft_check(&pass, &fail, "4c: RECOVERY_REQUIRED -> retry_recovery() succeeds -> VALID, composited", ok);

    // ── Cleanup: the M10FT_IDENTITY slot must still be releasable (no leak from any of the above). ──
    // Ensure we're not still claiming it as ACTIVE (we should be composited by now).
    display64_direct_scanout_unbind(M10FT_IDENTITY);

    klog("display64_atomic_fault_selftest: pass="); klog_hex("", (uint32_t)pass);
    klog("display64_atomic_fault_selftest: fail="); klog_hex("", (uint32_t)fail);
    return fail == 0;
}
#else
int display64_atomic_fault_selftest(void) {
    klog("display64_atomic_fault_selftest: M9B_FAULT_INJECT not compiled in -- SKIPPED\n");
    return 1;
}
#endif

int virtio_gpu64_init_atomic_backend(void) {
    if (display64_set_atomic_backend(atomic_check_impl, atomic_commit_impl) < 0) {
        klog("virtio_gpu64: M+10 atomic backend: display64_set_atomic_backend rejected\n");
        return -1;
    }
    klog("virtio_gpu64: M+10 atomic backend live\n");
    if (!display64_atomic_selftest()) {
        klog("virtio_gpu64: M+10 atomic self-test: FAILED (see display64_atomic_selftest's own per-case log above)\n");
    }
    if (!display64_atomic_fault_selftest()) {
        klog("virtio_gpu64: M+10 fault-injection self-test: FAILED (see display64_atomic_fault_selftest's own per-case log above)\n");
    }
    return 0;
}

// ── Cursor pixel-format self-test (current hardening pass, §7) ───────
// Proves the exact BYTES landing in the memobj-backed cursor resource
// -- g_cursor_px, the SAME memory TRANSFER_TO_HOST_2D reads from --
// match what was uploaded, byte for byte, with zero conversion applied
// anywhere in this path. Deliberately goes through the real public
// entry point (display64_cursor_set_image(), exactly what
// compositor64/SYS64_CURSOR_SET_IMAGE actually calls) rather than
// poking g_cursor_px directly, so this proves the ACTUAL production
// code path, not a hand-rolled shortcut. Safe to run at boot: whatever
// test image this uploads is unconditionally overwritten by
// compositor64's own real cursor image moments later (userspace hasn't
// even started yet when this runs), so there is nothing to restore
// afterward.
int virtio_gpu64_cursor_pixel_selftest(void) {
    if (!g_cursor_ready) {
        klog("virtio_gpu64_cursor_pixel_selftest: no cursor backend -- skipping\n");
        return 1; // absence is a first-class outcome, same precedent every other selftest here uses
    }

    static uint32_t test_img[VIRTIO_GPU64_CURSOR_DIM * VIRTIO_GPU64_CURSOR_DIM];
    // Six known logical 0xAARRGGBB pixels at fixed indices; the rest of
    // the image filled with a distinct sentinel so an indexing/stride
    // bug (not just a channel swap) would also be caught.
    const uint32_t known[6] = {
        0xFFFF0000u, // opaque red
        0xFF00FF00u, // opaque green
        0xFF0000FFu, // opaque blue
        0xFFFFFF00u, // opaque yellow
        0xFFFFFFFFu, // opaque white
        0x00000000u, // fully transparent
    };
    for (uint32_t i = 0; i < VIRTIO_GPU64_CURSOR_DIM * VIRTIO_GPU64_CURSOR_DIM; i++) test_img[i] = 0x00112233u;
    for (int i = 0; i < 6; i++) test_img[i] = known[i];

    if (display64_cursor_set_image(VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM, test_img, 0, 0) < 0) {
        klog("virtio_gpu64_cursor_pixel_selftest: display64_cursor_set_image FAILED\n");
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < 6; i++) {
        if (g_cursor_px[i] != known[i]) {
            klog_hex("virtio_gpu64_cursor_pixel_selftest: mismatch at index (see next line) got=", g_cursor_px[i]);
            klog_hex("  expected=", known[i]);
            ok = 0;
        }
    }
    if (g_cursor_px[6] != 0x00112233u) {
        klog("virtio_gpu64_cursor_pixel_selftest: sentinel region corrupted -- indexing bug\n");
        ok = 0;
    }
    klog(ok ? "virtio_gpu64_cursor_pixel_selftest: PASS -- exact bytes reached the cursor resource\n"
            : "virtio_gpu64_cursor_pixel_selftest: FAIL\n");
    return ok;
}

int virtio_gpu64_init_cursor(void) {
    if (!g_virtio_gpu_registered) return -1;
    if (g_cursor_buf) {
        klog("virtio_gpu64: cursor already initialized -- skipping\n");
        return -1;
    }
    if (!toxenos_virtio_gpu_cursor_available()) {
        klog("virtio_gpu64: cursor virtqueue not available -- hardware cursor disabled, software cursor remains\n");
        return -1;
    }

    uint64_t size_bytes = (uint64_t)VIRTIO_GPU64_CURSOR_DIM * VIRTIO_GPU64_CURSOR_DIM * 4;
    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(&g_virtio_gpu_device, size_bytes, GPU64_USAGE_CURSOR | GPU64_USAGE_CPU_WRITE, &buf) < 0) {
        klog("virtio_gpu64: cursor: gpu64_buffer_create failed\n");
        return -1;
    }

    // Same "kernel's own always-mapped CPU view" technique the
    // compositor backend already uses above -- no duplicate physical
    // storage, no second mapping.
    memobj64_run_t runs[8];
    int n = memobj64_get_runs(buf->backing, runs, 8);
    if (n != 1) {
        klog("virtio_gpu64: cursor: unexpected run count for a fresh contiguous buffer\n");
        gpu64_buffer_release(buf);
        return -1;
    }
    uint32_t* px = (uint32_t*)physmem64_to_virt(runs[0].phys);
    for (uint64_t i = 0; i < (uint64_t)VIRTIO_GPU64_CURSOR_DIM * VIRTIO_GPU64_CURSOR_DIM; i++) px[i] = 0x00000000u; // fully transparent -- safe even before compositor64 ever uploads a real image

    // Deliberately NOT virtio_gpu64_bind_scanout() -- that hardcodes the
    // no-alpha scanout format (VIRTIO_GPU64_FORMAT_B8G8R8X8_UNORM); the
    // cursor resource needs alpha, so its creation is inlined here with
    // the correct format instead.
    uint32_t resource_id;
    if (toxenos_virtio_gpu_resource_create_2d(VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_FORMAT_B8G8R8A8_UNORM, &resource_id) < 0) {
        klog("virtio_gpu64: cursor: resource_create_2d failed\n");
        gpu64_buffer_release(buf);
        return -1;
    }
    if (attach_memobj_backing(resource_id, buf->backing) < 0) {
        klog("virtio_gpu64: cursor: attach_backing failed\n");
        toxenos_virtio_gpu_resource_unref(resource_id);
        gpu64_buffer_release(buf);
        return -1;
    }
    // buf->driver_resource_id set manually (not via virtio_gpu64_bind_scanout)
    // -- virtio_gpu64_unbind_scanout() below still works correctly on it
    // regardless of which format the resource was created with; it only
    // ever calls DETACH_BACKING/RESOURCE_UNREF by resource ID, format-agnostic.
    buf->driver_resource_id = resource_id;

    if (toxenos_virtio_gpu_transfer_to_host_2d(resource_id, 0, 0, VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM, VIRTIO_GPU64_CURSOR_DIM) < 0) {
        klog("virtio_gpu64: cursor: initial transfer_to_host_2d failed\n");
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return -1;
    }

    g_cursor_buf = buf;
    g_cursor_px = px;
    g_cursor_ready = 1;

    if (display64_set_cursor_backend(cursor_backend_set_image, cursor_backend_move, cursor_backend_set_visible) < 0) {
        klog("virtio_gpu64: cursor: display64_set_cursor_backend rejected -- tearing down\n");
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        g_cursor_buf = 0;
        g_cursor_px = 0;
        g_cursor_ready = 0;
        return -1;
    }

    g_virtio_gpu_device.capability_flags |= GPU64_CAP_CURSOR_PLANE;
    klog("virtio_gpu64: hardware cursor backend live -- awaiting compositor cursor image upload\n");
    return 0;
}

// ── Item 9: isolated scanout proof ───────────────────────────────────
static void paint_color_bars(uint32_t* px, uint32_t width, uint32_t height) {
    // 8 vertical color bars, standard broadcast-test-pattern order,
    // packed as ToxenOS's own logical 0x00RRGGBB (== VirtIO's
    // B8G8R8X8_UNORM byte layout on this little-endian host -- see
    // this file's own header comment).
    static const uint32_t bars[8] = {
        0x00FFFFFF, 0x00FFFF00, 0x0000FFFF, 0x0000FF00,
        0x00FF00FF, 0x00FF0000, 0x000000FF, 0x00000000,
    };
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t bar = (x * 8) / width;
            if (bar > 7) bar = 7;
            px[(uint64_t)y * width + x] = bars[bar];
        }
    }
}

gpu64_buffer_t* virtio_gpu64_run_scanout_test(uint32_t width, uint32_t height) {
    if (!g_virtio_gpu_registered || width == 0 || height == 0) return 0;
    // M+4: refuse once a real compositor backend owns scanout 0 --
    // this disposable test would otherwise steal the display away from
    // it (and then leave scanout cleared, not restored, on its own
    // teardown). Absence of this guard would be a real regression: M+3
    // had nothing live to protect against.
    if (g_compositor_buf) {
        klog("virtio_gpu64: scanout test: refusing -- would steal scanout from the live compositor backend\n");
        return 0;
    }
    // Overflow-checked size computation, per this milestone's own
    // instruction never to skip this: width*height*4 must not wrap a
    // uint64_t (it never will for any realistic display size, but the
    // check costs nothing and this is exactly the kind of arithmetic
    // §5 warns about getting wrong).
    uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels == 0 || pixels > (0xFFFFFFFFFFFFFFFFULL / 4)) return 0;
    uint64_t size_bytes = pixels * 4;

    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(&g_virtio_gpu_device, size_bytes, GPU64_USAGE_SCANOUT | GPU64_USAGE_CPU_WRITE, &buf) < 0) {
        klog("virtio_gpu64: scanout test: gpu64_buffer_create failed\n");
        return 0;
    }

    // Paint the test pattern directly into the backing's own physical
    // memory -- memobj64_get_runs() again, never base_phys/npages read
    // directly (this file's own convention, matching Rust's own memobj
    // discipline one layer up).
    memobj64_run_t runs[8];
    int n = memobj64_get_runs(buf->backing, runs, 8);
    if (n != 1) {
        // Today's memobj64 only ever produces one contiguous run; a
        // count other than 1 means either failure or a future
        // scatter-gather backing this simple test-pattern writer isn't
        // prepared for -- fail cleanly rather than write past a run
        // boundary.
        klog("virtio_gpu64: scanout test: unexpected run count for a fresh contiguous buffer\n");
        gpu64_buffer_release(buf);
        return 0;
    }
    uint32_t* px = (uint32_t*)physmem64_to_virt(runs[0].phys);
    paint_color_bars(px, width, height);

    if (virtio_gpu64_bind_scanout(buf, width, height) < 0) {
        klog("virtio_gpu64: scanout test: bind_scanout failed\n");
        gpu64_buffer_release(buf);
        return 0;
    }

    if (toxenos_virtio_gpu_transfer_to_host_2d(buf->driver_resource_id, 0, 0, width, height, width, height) < 0) {
        klog("virtio_gpu64: scanout test: transfer_to_host_2d failed\n");
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return 0;
    }

    if (toxenos_virtio_gpu_set_scanout(0, buf->driver_resource_id, 0, 0, width, height) < 0) {
        klog("virtio_gpu64: scanout test: set_scanout failed\n");
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return 0;
    }

    if (toxenos_virtio_gpu_resource_flush(buf->driver_resource_id, 0, 0, width, height, width, height) < 0) {
        klog("virtio_gpu64: scanout test: resource_flush failed\n");
        // Scanout is already set at this point -- clearing it before
        // tearing down avoids leaving the device pointed at a resource
        // this driver is about to unref.
        toxenos_virtio_gpu_set_scanout(0, 0, 0, 0, 1, 1);
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);
        return 0;
    }

    klog("virtio_gpu64: scanout test: color bars flushed to scanout 0 -- check the display\n");
    return buf; // left bound and flushed -- caller's own responsibility to tear down afterward
}

// ── Self-test ─────────────────────────────────────────────────────
static int test_display_info(void) {
    virtio_gpu64_display_mode_t modes[VIRTIO_GPU64_MAX_SCANOUTS];
    int enabled = virtio_gpu64_get_display_info(modes);
    if (enabled < 0) return 0;
    int ok = 1;
    int saw_enabled = 0;
    for (int i = 0; i < VIRTIO_GPU64_MAX_SCANOUTS; i++) {
        if (!modes[i].enabled) continue;
        saw_enabled = 1;
        if (modes[i].width == 0 || modes[i].height == 0) ok = 0;
    }
    return ok && saw_enabled;
}

static int test_create_attach_transfer_flush_scanout_destroy(void) {
    gpu64_buffer_t* buf = virtio_gpu64_run_scanout_test(64, 64);
    if (!buf) return 0;
    int ok = (buf->driver_resource_id != 0);

    // Clear the scanout before destroying the resource it points at --
    // same ordering discipline the scanout test's own failure paths use.
    toxenos_virtio_gpu_set_scanout(0, 0, 0, 0, 1, 1);
    virtio_gpu64_unbind_scanout(buf);
    if (buf->driver_resource_id != 0) ok = 0; // must be cleared back to 0
    gpu64_buffer_release(buf);
    return ok;
}

static int test_invalid_resource_and_rect(void) {
    int ok = 1;
    // A resource ID that was never created -- must fail cleanly, not
    // crash or hang.
    if (toxenos_virtio_gpu_transfer_to_host_2d(0xFFFFFFFFu, 0, 0, 1, 1, 1, 1) == 0) ok = 0;
    if (toxenos_virtio_gpu_resource_flush(0xFFFFFFFFu, 0, 0, 1, 1, 1, 1) == 0) ok = 0;
    // A real resource, but a rect that doesn't fit it.
    uint32_t rid;
    if (toxenos_virtio_gpu_resource_create_2d(16, 16, VIRTIO_GPU64_FORMAT_B8G8R8X8_UNORM, &rid) < 0) return 0;
    if (toxenos_virtio_gpu_transfer_to_host_2d(rid, 10, 10, 10, 10, 16, 16) == 0) ok = 0; // 10+10 > 16
    toxenos_virtio_gpu_resource_unref(rid);
    return ok;
}

int virtio_gpu64_selftest(void) {
    int pass = 0, fail = 0;
    klog("virtio_gpu64_selftest: starting\n");

    if (!g_virtio_gpu_registered) {
        klog("virtio_gpu64_selftest: no VirtIO-GPU device initialized -- skipping\n");
        return 1; // not a failure -- see this file's own header comment: absence is a first-class outcome
    }

    int r;
    r = test_display_info();
    klog("virtio_gpu64_selftest: GET_DISPLAY_INFO reports at least one enabled scanout ");
    klog(r ? "PASS\n" : "FAIL\n"); if (r) pass++; else fail++;

    if (g_compositor_buf) {
        // M+4: this sub-test drives its own SET_SCANOUT round trip,
        // which would hijack (and then fail to restore) the live
        // production compositor's scanout -- see
        // virtio_gpu64_run_scanout_test()'s own guard. Skipped, not
        // failed: this is an expected, correct refusal in production
        // graphical boot, exactly the same "absence is a first-class
        // outcome" discipline this function already applies to a
        // missing device entirely.
        klog("virtio_gpu64_selftest: create+attach+transfer+flush+scanout+destroy round trip SKIPPED (live compositor backend owns scanout)\n");
    } else {
        r = test_create_attach_transfer_flush_scanout_destroy();
        klog("virtio_gpu64_selftest: create+attach+transfer+flush+scanout+destroy round trip ");
        klog(r ? "PASS\n" : "FAIL\n"); if (r) pass++; else fail++;
    }

    r = test_invalid_resource_and_rect();
    klog("virtio_gpu64_selftest: invalid resource id / out-of-bounds rect rejected ");
    klog(r ? "PASS\n" : "FAIL\n"); if (r) pass++; else fail++;

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("virtio_gpu64_selftest: pass="); klog(passbuf);
    klog(" fail="); klog(failbuf); klog("\n");
    return fail == 0;
}

// ── Item 15: honest performance comparison ───────────────────────────
// Software path: the SAME per-row display64_blit_row() loop
// sys64_display_present's own implementation uses (kernel/syscall64.c),
// called directly here rather than through the syscall (syscall entry
// overhead is not what this comparison is about). VirtIO-GPU path: one
// bound resource, TRANSFER_TO_HOST_2D + RESOURCE_FLUSH repeated. Both
// timed via the same timer64_get_ticks() (100Hz, 10ms/tick) M-next's
// own frame-pacing work already established as this codebase's smallest
// available time source. Logs raw tick counts for both -- deliberately
// not a synthetic "X% faster/slower" claim, since 10ms tick granularity
// is coarse enough that the honest thing to report is the raw counts an
// enough-iterations run over, not a false-precision derived percentage.
void virtio_gpu64_perf_compare(uint32_t width, uint32_t height, uint32_t iterations) {
    if (!g_virtio_gpu_registered || width == 0 || height == 0 || iterations == 0) {
        klog("virtio_gpu64: perf compare: no device or invalid parameters -- skipping\n");
        return;
    }

    uint32_t* row = (uint32_t*)kmalloc((uint64_t)width * 4);
    if (!row) {
        klog("virtio_gpu64: perf compare: row buffer allocation failed\n");
        return;
    }
    for (uint32_t x = 0; x < width; x++) row[x] = 0x00808080; // arbitrary -- timing only, not visual correctness

    uint64_t sw_start = timer64_get_ticks();
    for (uint32_t iter = 0; iter < iterations; iter++) {
        for (uint32_t y = 0; y < height; y++) display64_blit_row(0, y, width, row);
    }
    uint64_t sw_end = timer64_get_ticks();
    kfree(row);

    uint64_t size_bytes = (uint64_t)width * (uint64_t)height * 4;
    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(&g_virtio_gpu_device, size_bytes, GPU64_USAGE_SCANOUT | GPU64_USAGE_CPU_WRITE, &buf) < 0) {
        klog("virtio_gpu64: perf compare: gpu64_buffer_create failed -- software result only\n");
        goto report;
    }
    if (virtio_gpu64_bind_scanout(buf, width, height) < 0) {
        klog("virtio_gpu64: perf compare: bind_scanout failed -- software result only\n");
        gpu64_buffer_release(buf);
        buf = 0;
        goto report;
    }

    {
        int failed = 0;
        uint64_t gpu_start = timer64_get_ticks();
        for (uint32_t iter = 0; iter < iterations; iter++) {
            if (toxenos_virtio_gpu_transfer_to_host_2d(buf->driver_resource_id, 0, 0, width, height, width, height) < 0) { failed = 1; break; }
            if (toxenos_virtio_gpu_resource_flush(buf->driver_resource_id, 0, 0, width, height, width, height) < 0) { failed = 1; break; }
        }
        uint64_t gpu_end = timer64_get_ticks();

        toxenos_virtio_gpu_set_scanout(0, 0, 0, 0, 1, 1); // clear before teardown
        virtio_gpu64_unbind_scanout(buf);
        gpu64_buffer_release(buf);

        char b1[24], b2[24];
        klog("virtio_gpu64: perf compare ("); dec_to_str_local(width, b1); klog(b1);
        klog("x"); dec_to_str_local(height, b1); klog(b1);
        klog(", "); dec_to_str_local(iterations, b1); klog(b1); klog(" iterations):\n");

        klog("  software present path:      "); dec_to_str_local(sw_end - sw_start, b1); klog(b1);
        klog(" ticks (10ms each)\n");

        if (!failed) {
            klog("  VirtIO-GPU transfer+flush:  "); dec_to_str_local(gpu_end - gpu_start, b1); klog(b1);
            klog(" ticks (10ms each)\n");
        } else {
            klog("  VirtIO-GPU transfer+flush path FAILED partway through -- result incomplete\n");
        }
        (void)b2;
        return;
    }

report:
    {
        char b1[24];
        klog("virtio_gpu64: perf compare -- software present path only: ");
        dec_to_str_local(sw_end - sw_start, b1); klog(b1); klog(" ticks (10ms each)\n");
    }
}

// ── M+4 investigation: high-resolution command-latency reporting ────
// Manual interactive testing found real lag (laggy mouse movement,
// laggy window dragging, laggy initial redraw) that this milestone's
// own 10ms-tick perf_compare() above could not explain -- 10ms is far
// too coarse to see individual command overhead. These use
// kernel/tsc64.c's RDTSC-based clock (calibrated against the same PIT
// this file already relies on for perf_compare, just converted through
// a much finer-grained cycle counter) against the Rust driver's own
// per-command counters (rust/toxenos_rs/src/virtio_gpu.rs's
// STAT_COMMANDS/STAT_POLL_ITERS_*/STAT_CYCLES_TOTAL).
void virtio_gpu64_debug_stats_reset(void) {
    toxenos_virtio_gpu_debug_stats_reset();
}

void virtio_gpu64_debug_stats_report(uint64_t window_ms) {
    uint64_t stats[4];
    if (toxenos_virtio_gpu_debug_stats(stats) < 0) {
        klog("virtio_gpu64: debug_stats: not available\n");
        return;
    }
    uint64_t commands = stats[0];
    uint64_t poll_iters_total = stats[1];
    uint64_t poll_iters_max = stats[2];
    uint64_t cycles_total = stats[3];
    uint64_t us_total = tsc64_cycles_to_us(cycles_total);

    char b[24];
    klog("virtio_gpu64: debug_stats ---\n");
    klog("  commands:            "); dec_to_str_local(commands, b); klog(b); klog("\n");
    klog("  poll_iters total:    "); dec_to_str_local(poll_iters_total, b); klog(b); klog("\n");
    klog("  poll_iters max:      "); dec_to_str_local(poll_iters_max, b); klog(b); klog("\n");
    klog("  poll_iters avg:      "); dec_to_str_local(commands ? poll_iters_total / commands : 0, b); klog(b); klog("\n");
    klog("  total blocked time:  "); dec_to_str_local(us_total, b); klog(b); klog(" us\n");
    klog("  avg time/command:    "); dec_to_str_local(commands ? us_total / commands : 0, b); klog(b); klog(" us\n");
    if (window_ms > 0) {
        // commands-per-second = commands * 1000 / window_ms, careful of
        // overflow for a large command count over a short window --
        // this codebase's existing arithmetic-safety discipline applied
        // to a diagnostic, not just production paths.
        uint64_t cps = (commands > (0xFFFFFFFFFFFFFFFFULL / 1000)) ? 0 : (commands * 1000) / (window_ms ? window_ms : 1);
        klog("  commands/sec (over window): "); dec_to_str_local(cps, b); klog(b); klog("\n");
    }
    klog("virtio_gpu64: debug_stats end ---\n");
}


// ── M+11C diagnostics ────────────────────────────────────────────────
// The boot self-check deliberately attempts ONE forbidden POLL drain per IRQ-owned queue; the driver must
// refuse and count it. The report separates these deliberate refusals from real (unexpected) violations.
static uint32_t g_expected_wrong_owner[2];
static void report_queue(const char* label, uint32_t which) {
    uint64_t st[19];
    if (toxenos_virtio_gpu_irq_stats(which, st) < 0) return;
    klog(label); klog("\n");
    klog_hex("  irq_count:           ", (uint32_t)st[0]);
    klog_hex("  completions_drained: ", (uint32_t)st[1]);
    klog_hex("  max_completions/irq: ", (uint32_t)st[2]);
    klog_hex("  empty_irqs:          ", (uint32_t)st[3]);
    klog_hex("  waits_woken:         ", (uint32_t)st[9]);
    klog_hex("  timeouts:            ", (uint32_t)st[10]);
    klog_hex("  polling_count:       ", (uint32_t)st[8]);
    klog_hex("  poll_drains:         ", (uint32_t)st[7]);
    klog_hex("  wrong_owner_count:   ", (uint32_t)st[6]);
    klog_hex("  wrong_owner_unexpected:", (uint32_t)(st[6] > g_expected_wrong_owner[which & 1] ? st[6] - g_expected_wrong_owner[which & 1] : 0));
    klog_hex("  stale_irqs:          ", (uint32_t)st[5]);
    klog_hex("  stale_completions:   ", (uint32_t)st[13]);
    klog_hex("  abandoned_reclaimed: ", (uint32_t)st[14]);
    klog_hex("  inflight_violations: ", (uint32_t)st[12]);
    if (st[17]) {
        klog_hex("  wake_latency avg us: ", (uint32_t)(tsc64_cycles_to_us(st[15] / st[17])));
        klog_hex("  wake_latency max us: ", (uint32_t)(tsc64_cycles_to_us(st[16])));
    }
    klog(st[18] == 1 ? "  owner: IRQ\n" : st[18] == 0 ? "  owner: POLL\n" : "  owner: fenced\n");
}

void virtio_gpu64_irq_report(void) {
    if (!toxenos_virtio_gpu_is_ready()) return;
    report_queue("virtio_gpu64: CONTROL queue irq counters ---", 0);
    report_queue("virtio_gpu64: CURSOR queue irq counters ---", 1);
}

int virtio_gpu64_owner_selfcheck(void) {
    int a = toxenos_virtio_gpu_test_wrong_owner_poll(0);
    int b = toxenos_virtio_gpu_test_wrong_owner_poll(1);
    if (a == 1) g_expected_wrong_owner[0]++;
    if (b == 1) g_expected_wrong_owner[1]++;
    return (a == 1 || a == -1) && (b == 1 || b == -1);
}

// A GPU request from a context that may not wait must be REFUSED BEFORE anything is submitted: once
// the doorbell rings the device owns the buffers and completes the command whether or not anyone
// waits. Issued inside a preempt-disabled section on each queue; then the device is given time to
// (wrongly) complete something. Returns 1 iff both were refused and no interrupt/completion followed.
int virtio_gpu64_wouldblock_selfcheck(void) {
    uint64_t c0[19], c1[19], u0[19], u1[19];
    if (toxenos_virtio_gpu_irq_stats(0, c0) < 0 || toxenos_virtio_gpu_irq_stats(1, u0) < 0) return 0;
    virtio_gpu64_display_mode_t modes[VIRTIO_GPU64_MAX_SCANOUTS];
    klog("virtio_irq64 selftest: the two WouldBlock errors below are the EXPECTED refusals (non-blockable context)\n");
    process64_preempt_disable();
    int rc_ctl = virtio_gpu64_get_display_info(modes);
    int rc_cur = toxenos_virtio_gpu_cursor_move(0, 0, 1, 1);
    process64_preempt_enable();
    for (uint64_t t0 = timer64_get_ticks(); timer64_get_ticks() < t0 + 2; ) __asm__ volatile ("pause");   // let a wrongly submitted command complete
    if (toxenos_virtio_gpu_irq_stats(0, c1) < 0 || toxenos_virtio_gpu_irq_stats(1, u1) < 0) return 0;
    return rc_ctl < 0 && rc_cur < 0 && c1[0] == c0[0] && c1[1] == c0[1] && u1[0] == u0[0] && u1[1] == u0[1];
}

// VIRTIO_IRQ_TEST_DROP_CTL_IRQ: mask the control-queue MSI-X entry so a real command's completion
// interrupt never arrives; the command must time out, the ring inspection must find the completion,
// and the device must be demoted (verified) to POLLING and complete it.
void virtio_gpu64_irq_test_drop_ctl(void) {
    virtio_irq_dev_t* d = virtio_irq_dev(VIRTIO_DEV_GPU);
    if (!d->set || d->mode != VIRTIO_IRQ_LIVE) { klog("virtio_gpu64: drop-ctl test: device not IRQ-live, skipped\n"); return; }
    klog("virtio_gpu64: TEST: masking the control-queue MSI-X entry, then issuing a command\n");
    int rc = irq64_disable(d->irq[0]);
    virtio_gpu64_display_mode_t modes[VIRTIO_GPU64_MAX_SCANOUTS];
    int cmd = virtio_gpu64_get_display_info(modes);
    klog_hex("virtio_gpu64: drop-ctl test: mask rc=", (uint32_t)rc);
    klog_hex("virtio_gpu64: drop-ctl test: command result=", (uint32_t)cmd);
    klog(d->mode == VIRTIO_IRQ_POLLING ? "virtio_gpu64: drop-ctl test: device demoted to POLLING and the command completed\n"
        : d->mode == VIRTIO_IRQ_FAULTED ? "virtio_gpu64: drop-ctl test: device FAULTED (demotion could not be verified)\n"
        : "virtio_gpu64: drop-ctl test: device still LIVE (unexpected)\n");
    // Device-wide rule: control and cursor queues always share ONE owner (never controlq=POLL / cursorq=IRQ).
    int oc = toxenos_virtio_gpu_owner(0), ou = toxenos_virtio_gpu_owner(1);
    klog_hex("virtio_gpu64: drop-ctl test: owner(control)=", (uint32_t)oc);
    klog_hex("virtio_gpu64: drop-ctl test: owner(cursor) =", (uint32_t)ou);
    klog(oc == ou ? "virtio_gpu64: drop-ctl test: ownership device-wide consistent\n"
                  : "virtio_gpu64: drop-ctl test: *** MIXED OWNERSHIP ACROSS QUEUES ***\n");
    if (d->mode == VIRTIO_IRQ_POLLING) {
        // Verified-silence, device-wide: ONE unmapped/masked queue is not proof for the device.
        int s0 = toxenos_virtio_gpu_read_selector(0), s1 = toxenos_virtio_gpu_read_selector(1);
        const irq64_desc_t* e0 = irq64_get_desc(d->irq[0]); const irq64_desc_t* e1 = irq64_get_desc(d->irq[1]);
        klog_hex("virtio_gpu64: drop-ctl test: selector(control)=", (uint32_t)s0);
        klog_hex("virtio_gpu64: drop-ctl test: selector(cursor) =", (uint32_t)s1);
        klog((s0 == 0xFFFF && s1 == 0xFFFF) ? "virtio_gpu64: drop-ctl test: every queue selector reads NO_VECTOR\n"
                                            : "virtio_gpu64: drop-ctl test: *** A QUEUE SELECTOR IS STILL MAPPED ***\n");
        klog((e0 && e1 && e0->masked && e1->masked) ? "virtio_gpu64: drop-ctl test: every MSI-X entry masked\n"
                                                    : "virtio_gpu64: drop-ctl test: not every MSI-X entry masked (proof P2: set quiesced)\n");
    }
}
