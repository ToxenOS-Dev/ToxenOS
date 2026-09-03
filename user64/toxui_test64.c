// ToxenOS/user64/toxui_test64.c — Milestone 33: thin ring3 driver for
// userlib/toxui's own self-test suite (tox_selftest.c). Runs entirely
// single-process (PNG decoding, drawing, font rasterization are all
// ordinary computation, no compositor/display/input needed) -- driven
// by kernel/kernel64.c's TOXUI_TEST64_RUN debug flag, same
// process64_spawn+process64_wait pattern every other *_test64.c uses.
// Requires `make populate PACKAGE_DEBUG64=1` (for both this binary and
// the /toxui_test_assets/ PNG fixtures tox_selftest.c reads).
#include <stdint.h>
#include "tox64.h"
#include "tox_selftest.h"

void _start(void) {
    int ok = tox_selftest();
    sys_exit(ok ? 42 : 1);
    for (;;) { }
}
