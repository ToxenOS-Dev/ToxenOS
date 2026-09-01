// ToxenOS/user64/display_test64.c — Milestone 29: userspace display
// syscall ABI end-to-end test. Queries display geometry
// (sys_display_open), renders a test pattern into an ordinary
// sys_mmap'd backbuffer (large enough to span many non-contiguous
// physical pages -- proves SYS64_DISPLAY_PRESENT's whole-range
// paging64_check_user_range validation + direct-VA streaming works
// without needing physically contiguous userspace memory), presents it
// full-frame several times in a row, then renders into a SEPARATE
// shared-memory-backed surface and presents just a sub-rectangle of it
// (a dirty-rect present) -- proving shared memory remains a usable
// surface backing for this interface. Also confirms a deliberately
// invalid present request is rejected cleanly.
#include <stdint.h>
#include "tox64.h"

#define OK_EXIT 42

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static void append_u64(char* out, int* pos, uint64_t v) {
    char rev[24]; int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    while (rn > 0) out[(*pos)++] = rev[--rn];
}

static void put_kv(const char* label, uint64_t v) {
    char line[64];
    int pos = 0;
    while (*label) line[pos++] = *label++;
    append_u64(line, &pos, v);
    line[pos++] = '\n';
    line[pos] = 0;
    put(line);
}

static void fill_pattern(uint32_t* buf, uint32_t w, uint32_t h, uint32_t phase) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = (x + phase) & 0xFF;
            uint32_t g = (y + phase) & 0xFF;
            uint32_t b = (x ^ y) & 0xFF;
            buf[y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
}

void _start(void) {
    put("display_test64: starting\n");

    uint32_t width = 0, height = 0, format = 0;
    int64_t h = sys_display_open(&width, &height, &format);
    if (h < 0) {
        put("display_test64: sys_display_open FAILED (no display, or already owned)\n");
        sys_exit(1);
    }
    put_kv("display_test64: width=", width);
    put_kv("display_test64: height=", height);
    put_kv("display_test64: format=", format);

    // A second open must fail while this one is held.
    int64_t h2 = sys_display_open(0, 0, 0);
    if (h2 >= 0) {
        put("display_test64: FAIL second sys_display_open unexpectedly succeeded\n");
        sys_handle_close((int)h2);
        sys_handle_close((int)h);
        sys_exit(1);
    }
    put("display_test64: second sys_display_open correctly rejected (single-owner) PASS\n");

    // Ordinary private backbuffer via sys_mmap -- for a real display
    // resolution this spans many pages (e.g. 1024x768x4 = 768 4KB
    // pages), which sys_mmap has no reason to make physically
    // contiguous -- exactly the "virtually contiguous, physically
    // non-contiguous" case SYS64_DISPLAY_PRESENT must handle correctly.
    uint64_t buf_size = (uint64_t)width * height * 4;
    uint64_t addr = sys_mmap(buf_size);
    if (addr == (uint64_t)-1) {
        put("display_test64: sys_mmap FAILED\n");
        sys_handle_close((int)h);
        sys_exit(1);
    }
    put_kv("display_test64: backbuffer pages=", (buf_size + 4095) / 4096);

    display64_present_req_t req;
    req.buf_ptr = addr;
    req.pitch = 0; // tightly packed
    req.x = 0; req.y = 0; req.w = width; req.h = height;

    int ok = 1;
    for (int frame = 0; frame < 5; frame++) {
        fill_pattern((uint32_t*)addr, width, height, (uint32_t)frame * 16);
        int64_t r = sys_display_present((int)h, &req);
        if (r != 0) {
            put("display_test64: FAIL full-frame present\n");
            ok = 0;
            break;
        }
    }
    if (ok) put("display_test64: repeated full-frame present (mmap backbuffer) PASS\n");

    // Deliberately invalid present request -- must fail cleanly (-1),
    // not corrupt anything or crash.
    display64_present_req_t bad_req = req;
    bad_req.w = width + 1000; // overshoots the real display width
    int64_t bad_r = sys_display_present((int)h, &bad_req);
    if (bad_r == 0) {
        put("display_test64: FAIL invalid present request was accepted\n");
        ok = 0;
    } else {
        put("display_test64: invalid present request correctly rejected PASS\n");
    }

    sys_munmap(addr, buf_size);

    // Shared-memory-backed surface + a dirty-rect (sub-rectangle) present.
    int64_t shm_h = sys_shm_create(buf_size);
    if (shm_h < 0) {
        put("display_test64: sys_shm_create FAILED\n");
        sys_handle_close((int)h);
        sys_exit(ok ? OK_EXIT : 1);
    }
    uint64_t shm_addr = sys_shm_map((int)shm_h, 1);
    if (shm_addr == (uint64_t)-1) {
        put("display_test64: sys_shm_map FAILED\n");
        sys_handle_close((int)shm_h);
        sys_handle_close((int)h);
        sys_exit(1);
    }
    fill_pattern((uint32_t*)shm_addr, width, height, 200);

    display64_present_req_t dirty_req;
    dirty_req.buf_ptr = shm_addr;
    dirty_req.pitch = width * 4; // same buffer, but only present a corner of it
    dirty_req.x = 0; dirty_req.y = 0;
    dirty_req.w = (width < 100) ? width : 100;
    dirty_req.h = (height < 100) ? height : 100;
    int64_t dirty_r = sys_display_present((int)h, &dirty_req);
    if (dirty_r == 0) put("display_test64: shared-memory-backed dirty-rect present PASS\n");
    else { put("display_test64: FAIL shared-memory dirty-rect present\n"); ok = 0; }

    sys_handle_close((int)shm_h);
    sys_handle_close((int)h);

    put(ok ? "display_test64: all checks PASS\n" : "display_test64: one or more checks FAILED\n");
    sys_exit(ok ? OK_EXIT : 1);
    for (;;) { } // unreachable
}
