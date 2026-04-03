// user/bin/sleeptest.c — tests sleep and scheduler blocking
#include "../tox.h"

void _start()
{
    print("Sleep test\n");
    print("----------\n");

    set_color(0x0A);
    print("Sleeping 1 second...\n");
    set_color(0x07);
    tox_sleep(1000);
    print("  1s done\n");

    set_color(0x0A);
    print("Sleeping 2 seconds...\n");
    set_color(0x07);
    tox_sleep(2000);
    print("  2s done\n");

    set_color(0x0A);
    print("Sleeping 3 seconds...\n");
    set_color(0x07);
    tox_sleep(3000);
    print("  3s done\n");

    set_color(0x0E);
    print("All sleeps passed!\n");
    set_color(0x07);
    print("(cursor should have kept blinking during each sleep)\n");

    tox_exit();
}
