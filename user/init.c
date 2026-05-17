// ToxenOS/user/init.c — PID 1
// Launches the interactive shell directly. No service manager.
#include "tox.h"

void _start()
{
    tox_spawn_embedded(0);
    while (1) yield();
}
