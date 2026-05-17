// ToxenOS/user/init.c — PID 1
// Launches the interactive shell directly. No service manager.
#include "tox.h"

void _start()
{
    // Set default environment variables inherited by all child processes
    tox_setenv("PATH",  "/System/bin:/");
    tox_setenv("HOME",  "/");
    tox_setenv("SHELL", "/shell.elf");
    tox_setenv("TERM",  "toxterm");
    tox_setenv("OS",    "ToxenOS");

    tox_spawn_embedded(0);
    while (1) yield();
}
