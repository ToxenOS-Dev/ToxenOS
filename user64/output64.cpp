// ToxenOS/user64/output64.cpp — M+12H: the ONE compositor-internal
// Output abstraction implementation. See output64.h for the full C
// boundary compositor64.c (a .c file) consumes; this is the only file
// that still calls sys_display_*/sys_cursor_* directly -- compositor64.c
// no longer assumes "there is exactly this one physical display accessed
// through these exact sys_display_* calls," it goes through this named
// boundary instead. display64/gpu64/VirtIO-GPU (kernel-side) are
// completely untouched; this is a thin userspace-only client of the same
// syscalls compositor64.c already used, not a new subsystem.
//
// Audit (M+12H design phase): every sys_display_*/sys_cursor_* call site
// in compositor64.c was catalogued and split into MECHANISM (moves here)
// vs POLICY (stays in compositor64.c). Scanout eligibility
// (direct_scanout_eligible()), the presented-mode/direct-window state
// machine (g_presented_mode/g_direct_window_idx), the hardware-vs-
// software cursor runtime decision (g_hw_cursor_active), and all
// software-cursor pixel rendering (bb_draw_cursor()/
// build_hw_cursor_image()) all stay in compositor64.c -- Output makes no
// eligibility or fallback decisions of its own, it only executes the
// operation it's asked to perform. This mirrors the EXISTING
// direct-scanout architectural rule (compositor owns policy, the lower
// mechanism executes) applied to a second case (hardware cursor) that
// has the identical shape.
//
// M+12H requirement 1 (preserve direct-capability query timing):
// output64_init() ONLY opens the display and records dimensions/
// ownership -- it does NOT query sys_display_direct_query() at all.
// output64_direct_supported() performs that query fresh, every call, at
// whatever point the caller (compositor64.c's own _start(), at its
// existing later call site, after cursor setup) chooses -- exactly
// preserving today's ordering. Output therefore holds no
// direct_supported_ field; there is nothing to cache.
//
// M+12H requirement 2 (close ownership semantics, proven not assumed):
// kernel/syscall64.c's sys64_handle_close() switches on the handle's
// kind; for HANDLE64_DISPLAY it calls display64_release() (declared
// `void display64_release(void)` in include/display64.h -- it cannot
// itself signal failure) and then UNCONDITIONALLY falls through to mark
// the handle slot UNUSED and return 0. The only paths that return -1 are
// "no current process," "handle index out of range," or "handle kind
// unrecognized" -- none of which can happen here while initialized_ is
// true (it is only ever true after a successful open of this exact
// handle number, and nothing else in this process touches it). Closing
// a validly-owned display handle therefore always succeeds
// deterministically from userspace's point of view -- Output::close()
// below is the simple, unconditional path the M+12H design explicitly
// allows for exactly this proven contract. Re-verify this comment
// against kernel/syscall64.c's sys64_handle_close() if its
// HANDLE64_DISPLAY case (or display64_release()'s signature) ever
// changes.
#include "output64.h"
#include "tox64.h"

namespace {

// M+12H: a single physical output -- exactly one instance, for the
// compositor process's whole lifetime (matches how g_display_h was a
// bare global before this milestone). Deliberately trivial: no in-class
// member initializers, no virtual functions, no base classes, no
// user-provided constructor/destructor body -- the static_asserts below
// (compile-time) and output64.o's own object-file inspection
// (readelf -S/nm -u/readelf -d, run at implementation-verification time)
// both confirm no .init_array/.fini_array/__cxa_*/RTTI/vtable machinery
// is ever emitted for it. All real initialization happens explicitly in
// init(), never in the constructor -- see this project's own C++ rules
// on preferring explicit init over constructor side effects.
//
// initialized_ is the ONLY lifetime guard. handle_'s numeric value --
// including 0, a valid ToxenOS handle -- is NEVER consulted, compared,
// or trusted except by the one sys_handle_close(handle_) call inside
// close(), which is only reachable once initialized_ already proves
// ownership. This is exactly the invariant the earlier has_surface bug
// violated (a reused/zero-valued field silently treated as "nothing to
// do" and closing handle 0 out from under the compositor) --
// initialized_ is a real, dedicated boolean, never reused for anything
// else, and no handle-value sentinel is relied on anywhere.
class Output {
public:
    Output() = default;
    ~Output() = default;

    bool init(uint32_t* width_out, uint32_t* height_out);
    void close();

    int64_t present_rect(const uint32_t* pixels, uint32_t stride_pixels,
                          int32_t x, int32_t y, uint32_t w, uint32_t h) const;

    int64_t direct_supported() const;
    int64_t direct_bind(uint64_t shm_token, uint32_t w, uint32_t h) const;
    int64_t direct_present(uint64_t shm_token, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t switch_active) const;
    int64_t direct_leave() const;
    int64_t direct_unbind(uint64_t shm_token) const;

    int64_t cursor_available() const;
    int64_t cursor_set_image(uint32_t w, uint32_t h, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y) const;
    int64_t cursor_move(int32_t x, int32_t y) const;
    int64_t cursor_set_visible(uint32_t visible) const;

    int64_t debug_state(output64_debug_state_t* out) const;

private:
    bool     initialized_;
    int      handle_;
    uint32_t width_;
    uint32_t height_;
};

static_assert(__is_trivially_constructible(Output), "Output must stay trivially constructible -- no .init_array should ever be needed");
static_assert(__has_trivial_destructor(Output), "Output must stay trivially destructible -- no __cxa_atexit registration should ever be needed");

Output g_output; // .bss, zero-initialized by the loader -- see class comment above

} // namespace

bool Output::init(uint32_t* width_out, uint32_t* height_out) {
    if (initialized_) return false; // double-init: fail cleanly, touch nothing

    uint32_t w = 0, h = 0, fmt = 0;
    int64_t dh = sys_display_open(&w, &h, &fmt);
    if (dh < 0) return false; // failure: initialized_ stays false, nothing acquired, nothing to release

    handle_ = (int)dh; // ownership genuinely acquired now
    width_ = w;
    height_ = h;
    initialized_ = true; // set LAST, only after ownership is held

    if (width_out) *width_out = width_;
    if (height_out) *height_out = height_;
    return true;
}

void Output::close() {
    if (!initialized_) return; // harmless no-op -- handle_'s value is never read here
    sys_handle_close(handle_); // see this file's own top-of-file comment: proven, not assumed, always succeeds here
    initialized_ = false;
    handle_ = -1; // defensive/debug-readability only -- NOT a safety sentinel; initialized_ is the only thing anything checks
    width_ = 0;
    height_ = 0;
}

int64_t Output::present_rect(const uint32_t* pixels, uint32_t stride_pixels,
                              int32_t x, int32_t y, uint32_t w, uint32_t h) const {
    if (!initialized_) return -1;
    display64_present_req_t req;
    req.buf_ptr = (uint64_t)(uintptr_t)pixels;
    req.pitch = stride_pixels * 4;
    req.x = x; req.y = y; req.w = w; req.h = h;
    return sys_display_present(handle_, &req);
}

int64_t Output::direct_supported() const {
    if (!initialized_) return 0; // matches sys_display_direct_query's own "0 = unsupported" contract
    return sys_display_direct_query(handle_);
}

int64_t Output::direct_bind(uint64_t shm_token, uint32_t w, uint32_t h) const {
    if (!initialized_) return -1;
    return sys_display_direct_bind(handle_, shm_token, w, h);
}

int64_t Output::direct_present(uint64_t shm_token, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t switch_active) const {
    if (!initialized_) return -1;
    return sys_display_direct_present(handle_, shm_token, x, y, w, h, switch_active);
}

int64_t Output::direct_leave() const {
    if (!initialized_) return -1;
    return sys_display_direct_leave(handle_);
}

int64_t Output::direct_unbind(uint64_t shm_token) const {
    if (!initialized_) return -1;
    return sys_display_direct_unbind(handle_, shm_token);
}

int64_t Output::cursor_available() const {
    if (!initialized_) return 0;
    return sys_cursor_available(handle_);
}

int64_t Output::cursor_set_image(uint32_t w, uint32_t h, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y) const {
    if (!initialized_) return -1;
    return sys_cursor_set_image(handle_, w, h, argb_pixels, hot_x, hot_y);
}

int64_t Output::cursor_move(int32_t x, int32_t y) const {
    if (!initialized_) return -1;
    return sys_cursor_move(handle_, x, y);
}

int64_t Output::cursor_set_visible(uint32_t visible) const {
    if (!initialized_) return -1;
    return sys_cursor_set_visible(handle_, visible);
}

int64_t Output::debug_state(output64_debug_state_t* out) const {
    if (!initialized_) return -1;
    display64_debug_state_req_t dbg;
    int64_t rc = sys_display_debug_state(handle_, &dbg);
    if (rc != 0) return rc;
    out->validity = dbg.validity;
    out->primary_kind = dbg.primary_kind;
    out->primary_identity = dbg.primary_identity;
    return 0;
}

extern "C" {

int output64_init(uint32_t* width_out, uint32_t* height_out) {
    return g_output.init(width_out, height_out) ? 1 : 0;
}
void output64_close(void) { g_output.close(); }

int64_t output64_present_rect(const uint32_t* pixels, uint32_t stride_pixels,
                               int32_t x, int32_t y, uint32_t w, uint32_t h) {
    return g_output.present_rect(pixels, stride_pixels, x, y, w, h);
}

int64_t output64_direct_supported(void) { return g_output.direct_supported(); }
int64_t output64_direct_bind(uint64_t shm_token, uint32_t w, uint32_t h) {
    return g_output.direct_bind(shm_token, w, h);
}
int64_t output64_direct_present(uint64_t shm_token, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t switch_active) {
    return g_output.direct_present(shm_token, x, y, w, h, switch_active);
}
int64_t output64_direct_leave(void) { return g_output.direct_leave(); }
int64_t output64_direct_unbind(uint64_t shm_token) { return g_output.direct_unbind(shm_token); }

int64_t output64_cursor_available(void) { return g_output.cursor_available(); }
int64_t output64_cursor_set_image(uint32_t w, uint32_t h, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y) {
    return g_output.cursor_set_image(w, h, argb_pixels, hot_x, hot_y);
}
int64_t output64_cursor_move(int32_t x, int32_t y) { return g_output.cursor_move(x, y); }
int64_t output64_cursor_set_visible(uint32_t visible) { return g_output.cursor_set_visible(visible); }

int64_t output64_debug_state(output64_debug_state_t* out) { return g_output.debug_state(out); }

} // extern "C"
