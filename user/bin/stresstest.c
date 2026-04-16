// stresstest.c — spawn/exit stress test to verify no memory leaks
// Spawns a simple process (echo) N times and waits for each one.
// If the kernel stack or process slot leaks, this will eventually fail.
#include "../tox.h"

void _start() {
    set_color(0x0B); print("=== Stress Test ===\n"); set_color(0x07);
    print("Spawning echo 50 times...\n");

    int ok = 0, fail = 0;
    for (int i = 0; i < 50; i++) {
        int pid = tox_spawn("/C:/Programs/echo.elf");
        if (pid < 0) {
            fail++;
            set_color(0x0C);
            print("FAIL at iteration ");
            // print iteration number
            char buf[8]; int n = i; int pos = 6;
            buf[7] = 0; buf[6] = '0' + (n % 10); n /= 10;
            buf[5] = n ? ('0' + n % 10) : ' '; n /= 10;
            buf[4] = n ? ('0' + n % 10) : ' ';
            print(buf + 4); print("\n");
            set_color(0x07);
            break;
        }
        tox_wait(pid);
        ok++;

        // Print progress every 10
        if ((i + 1) % 10 == 0) {
            print("  ");
            char buf[4]; buf[3] = 0;
            buf[0] = '0' + (i+1)/10;
            buf[1] = '0'; buf[2] = 0;
            print(buf); print(" done\n");
        }
    }

    if (fail == 0) {
        set_color(0x0A);
        print("All 50 spawns succeeded. No leaks detected.\n");
    } else {
        set_color(0x0C);
        print("FAILED after "); 
        char buf[4]; buf[3] = 0; buf[0] = '0' + ok/10; buf[1] = '0' + ok%10; buf[2] = 0;
        print(buf); print(" successes.\n");
    }
    set_color(0x07);
    tox_exit();
}
