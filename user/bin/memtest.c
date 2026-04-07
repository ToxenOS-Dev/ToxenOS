// user/bin/memtest.c — tests sbrk-backed malloc/free/realloc
#include "../tox.h"

void _start()
{
    print("Memory test\n");
    print("-----------\n");

    // Test 1: basic alloc/free
    set_color(0x0A); print("Test 1: basic alloc... "); set_color(0x07);
    char* p1 = malloc(64);
    if (!p1) { set_color(0x0C); print("FAIL (null)\n"); set_color(0x07); tox_exit(); }
    for (int i = 0; i < 64; i++) p1[i] = (char)i;
    int ok = 1;
    for (int i = 0; i < 64; i++) if (p1[i] != (char)i) { ok = 0; break; }
    set_color(ok ? 0x0A : 0x0C);
    print(ok ? "PASS\n" : "FAIL (corrupt)\n");
    set_color(0x07);
    free(p1);

    // Test 2: large allocation (forces sbrk)
    set_color(0x0A); print("Test 2: large alloc (512KB)... "); set_color(0x07);
    char* big = malloc(512 * 1024);
    if (!big) { set_color(0x0C); print("FAIL (null)\n"); set_color(0x07); tox_exit(); }
    // Write pattern across whole block
    for (uint32_t i = 0; i < 512*1024; i++) big[i] = (char)(i & 0xFF);
    ok = 1;
    for (uint32_t i = 0; i < 512*1024; i++)
        if (big[i] != (char)(i & 0xFF)) { ok = 0; break; }
    set_color(ok ? 0x0A : 0x0C);
    print(ok ? "PASS\n" : "FAIL (corrupt)\n");
    set_color(0x07);
    free(big);

    // Test 3: many small allocs
    set_color(0x0A); print("Test 3: 100 small allocs... "); set_color(0x07);
    void* ptrs[100];
    ok = 1;
    for (int i = 0; i < 100; i++) {
        ptrs[i] = malloc(128);
        if (!ptrs[i]) { ok = 0; break; }
        // write index into each block
        ((char*)ptrs[i])[0] = (char)i;
    }
    // verify and free
    for (int i = 0; i < 100; i++) {
        if (ptrs[i] && ((char*)ptrs[i])[0] != (char)i) ok = 0;
        if (ptrs[i]) free(ptrs[i]);
    }
    set_color(ok ? 0x0A : 0x0C);
    print(ok ? "PASS\n" : "FAIL\n");
    set_color(0x07);

    // Test 4: realloc
    set_color(0x0A); print("Test 4: realloc... "); set_color(0x07);
    char* r = malloc(32);
    if (r) {
        for (int i = 0; i < 32; i++) r[i] = (char)i;
        r = realloc(r, 256);
        ok = r != 0;
        if (ok) for (int i = 0; i < 32; i++) if (r[i] != (char)i) { ok = 0; break; }
        if (r) free(r);
    } else ok = 0;
    set_color(ok ? 0x0A : 0x0C);
    print(ok ? "PASS\n" : "FAIL\n");
    set_color(0x07);

    set_color(0x0E);
    print("Memory tests complete.\n");
    set_color(0x07);

    tox_exit();
}
