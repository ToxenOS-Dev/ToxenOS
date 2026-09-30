// ToxenOS/user64/output64.h — M+12H: compositor-internal Output
// abstraction, the C boundary. Pure C, safe to #include from
// compositor64.c (a .c file) or from the C++ implementation
// (output64.cpp) itself. See output64.cpp's own header comment for the
// full design writeup (audit, policy/mechanism split, C++ rationale,
// lifetime model).
//
// Scope: exactly ONE physical output. Every function here is a thin
// mechanism wrapper around the same sys_display_*/sys_cursor_* calls
// compositor64.c used to make directly -- see each function's own
// comment for exactly which one and what its return-value contract is.
// Scanout/cursor ELIGIBILITY POLICY (when to use direct scanout, when to
// fall back to software cursor) stays entirely in compositor64.c; this
// header exposes mechanism only.
#ifndef OUTPUT64_H
#define OUTPUT64_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opens the one physical output this process owns and records its
// dimensions. Must be called before any other output64_* call --- every
// other function fails cleanly (see each one's own contract) if called
// first. Returns 1 on success (width_out/height_out filled), 0 on
// failure: no display available, OR already initialized (double-init is
// a clean failure, not undefined behavior -- see output64.cpp's own
// Output::init()).
int output64_init(uint32_t* width_out, uint32_t* height_out);

// Releases the display handle. Harmless no-op if not currently
// initialized. See output64.cpp's own top-of-file comment for why this
// is a plain, unconditional release with no failure path -- proven
// against kernel/syscall64.c's actual sys64_handle_close() contract for
// HANDLE64_DISPLAY, not assumed.
void output64_close(void);

// Presents one rect already composited into `pixels` (row-major,
// `stride_pixels` uint32_t's per row) at (x,y,w,h) in output
// coordinates. Returns the RAW sys_display_present() result unchanged
// (0 = success, negative = failure) -- callers use whatever comparison
// they already used against that syscall's own contract. -1 if not
// initialized.
int64_t output64_present_rect(const uint32_t* pixels, uint32_t stride_pixels,
                               int32_t x, int32_t y, uint32_t w, uint32_t h);

// Direct-scanout MECHANISM only -- see compositor64.c's own
// direct_scanout_eligible()/direct_scanout_try_enter_or_update()/
// direct_scanout_force_leave() for the eligibility POLICY.
//
// output64_direct_supported() performs sys_display_direct_query() FRESH
// on every call -- deliberately NOT cached inside Output, and NOT
// queried inside output64_init() -- so the compositor's own existing
// call-site timing (queried later in its own startup sequence, after
// cursor setup, exactly as before M+12H) is completely unchanged.
// Returns the RAW query result unchanged (1 or 0 -- this is the
// capability ANSWER itself, not a generic success/failure code; see
// include/syscall64.h's own SYS64_DISPLAY_DIRECT_QUERY comment), or 0
// if not initialized (matches "unsupported").
int64_t output64_direct_supported(void);
int64_t output64_direct_bind(uint64_t shm_token, uint32_t w, uint32_t h);
int64_t output64_direct_present(uint64_t shm_token, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t switch_active);
int64_t output64_direct_leave(void);
int64_t output64_direct_unbind(uint64_t shm_token);

// Hardware cursor MECHANISM only -- see compositor64.c's own
// g_hw_cursor_active for the "should I be using it right now" POLICY
// and runtime hw->sw fallback decision, which stay entirely in the
// compositor. Each returns the RAW underlying sys_cursor_*() result
// unchanged.
int64_t output64_cursor_available(void);
int64_t output64_cursor_set_image(uint32_t w, uint32_t h, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y);
int64_t output64_cursor_move(int32_t x, int32_t y);
int64_t output64_cursor_set_visible(uint32_t visible);

// Diagnostic only -- see dump_window_liveness()'s own use. A tiny,
// backend-neutral translation of display64_debug_state_req_t, which
// never appears in this header -- see output64.cpp.
typedef struct {
    uint32_t validity;         // 0 = VALID, 1 = RECOVERY_REQUIRED
    uint32_t primary_kind;     // 0 = COMPOSITED, 1 = DIRECT
    uint64_t primary_identity; // DIRECT only -- 0 for COMPOSITED
} output64_debug_state_t;
int64_t output64_debug_state(output64_debug_state_t* out);

#ifdef __cplusplus
}
#endif
#endif // OUTPUT64_H
