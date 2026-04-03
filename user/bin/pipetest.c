// user/bin/pipetest.c — tests wait-queue pipe blocking
#include "../tox.h"

void _start()
{
    print("Pipe test\n");
    print("---------\n");

    // Test 1: create pipe and verify fds are valid
    set_color(0x0A); print("Test 1: pipe() returns valid fds... "); set_color(0x07);
    int rfd, wfd;
    int r = tox_pipe(&rfd, &wfd);
    if (r < 0 || rfd < 0 || wfd < 0 || rfd == wfd) {
        set_color(0x0C); print("FAIL\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x0A); print("PASS (rfd=");
    set_color(0x07); print_int(rfd);
    print(", wfd=");
    print_int(wfd);
    set_color(0x0A); print(")\n"); set_color(0x07);

    // Test 2: write then read
    set_color(0x0A); print("Test 2: write/read round-trip... "); set_color(0x07);
    const char* msg = "ToxenOS";
    tox_write(wfd, (const uint8_t*)msg, 7);
    tox_close(wfd);  // close write end so read doesn't block forever

    char buf[16];
    int  n = tox_read(rfd, (uint8_t*)buf, 15);
    tox_close(rfd);

    buf[n < 0 ? 0 : n] = 0;
    int ok = (n == 7);
    for (int i = 0; i < 7 && ok; i++) ok = (buf[i] == msg[i]);
    set_color(ok ? 0x0A : 0x0C);
    print(ok ? "PASS\n" : "FAIL\n");
    set_color(0x07);

    // Test 3: EOF on closed write end returns 0 bytes
    set_color(0x0A); print("Test 3: EOF detection... "); set_color(0x07);
    int rfd2, wfd2;
    tox_pipe(&rfd2, &wfd2);
    tox_close(wfd2);  // close immediately
    char tmp[4];
    int nr = tox_read(rfd2, (uint8_t*)tmp, 4);
    tox_close(rfd2);
    set_color(nr == 0 ? 0x0A : 0x0C);
    print(nr == 0 ? "PASS\n" : "FAIL\n");
    set_color(0x07);

    set_color(0x0E);
    print("Pipe tests complete.\n");
    set_color(0x07);

    tox_exit();
}
