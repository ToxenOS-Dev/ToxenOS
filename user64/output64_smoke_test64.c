// ToxenOS/user64/output64_smoke_test64.c — M+12H: exercises Output's
// own init/close lifecycle directly against the real display backend,
// several cycles, entirely on its own (no compositor connection, no WM
// protocol). Plain C -- output64.h's extern "C" boundary is fully
// includable from a .c file; this program never needs to be C++ itself.
//
// MUST run to completion, and fully release HANDLE64_DISPLAY, BEFORE
// compositor64 ever attempts to open it -- display is a singleton
// resource (include/handle64.h's own HANDLE64_DISPLAY comment) and the
// two cannot hold it concurrently. See user64/init64.c's own comment at
// this program's spawn site for the exact ordering guarantee (spawned
// and waited on, sequentially, before compositor64 is ever spawned).
// compositor64's own subsequent successful sys_display_open() is the
// strongest independent proof that this test's own close() calls never
// leaked ownership.
//
// Each cycle: init (dimensions must be nonzero and identical across
// every cycle -- the same one physical output every time) -> present one
// small test rect (must succeed) -> close -> repeat. A second init()
// succeeding at all is itself proof the PRIOR close() genuinely released
// the singleton handle (see output64.h's own documented double-init
// contract: init() while already initialized fails cleanly, so if close()
// had ever failed to release ownership, every init() after the first
// would fail here and this test would correctly FAIL instead of
// silently passing).
#include <stdint.h>
#include "tox64.h"
#include "output64.h"

#define CYCLES 5
#define TEST_RECT_DIM 8

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

void _start(void) {
    put("output64_smoke_test64: starting\n");

    uint32_t first_w = 0, first_h = 0;
    uint32_t pixels[TEST_RECT_DIM * TEST_RECT_DIM];
    for (int i = 0; i < TEST_RECT_DIM * TEST_RECT_DIM; i++) pixels[i] = 0x00445566u;

    for (int cycle = 0; cycle < CYCLES; cycle++) {
        uint32_t w = 0, h = 0;
        if (output64_init(&w, &h) != 1) {
            put("output64_smoke_test64: output64_init FAILED on cycle\n");
            sys_exit(1);
        }
        if (w == 0 || h == 0) {
            put("output64_smoke_test64: output64_init reported zero dimensions\n");
            sys_exit(1);
        }
        if (cycle == 0) {
            first_w = w; first_h = h;
        } else if (w != first_w || h != first_h) {
            put("output64_smoke_test64: dimensions changed across cycles\n");
            sys_exit(1);
        }

        // A second init() before closing the first must fail cleanly
        // (double-init contract) -- proves initialized_ genuinely guards
        // against double-acquiring the singleton, not just that a fresh
        // process happens to succeed once.
        uint32_t dummy_w = 0, dummy_h = 0;
        if (output64_init(&dummy_w, &dummy_h) != 0) {
            put("output64_smoke_test64: double-init unexpectedly SUCCEEDED\n");
            sys_exit(1);
        }

        int64_t present_rc = output64_present_rect(pixels, TEST_RECT_DIM, 0, 0, TEST_RECT_DIM, TEST_RECT_DIM);
        if (present_rc < 0) {
            put("output64_smoke_test64: output64_present_rect FAILED\n");
            sys_exit(1);
        }

        output64_close();
    }

    // Closing when already uninitialized must be a harmless no-op, not
    // a crash or a hang -- exercise it explicitly once, after the last
    // real close() above already ran.
    output64_close();

    put("output64_smoke_test64: PASS\n");
    sys_exit(42);
}
