#include "../tox.h"

void _start() {
    char args[64]; tox_get_args(args);
    if (!args[0]) { print("Usage: kill [-9] <pid>\n"); tox_exit(); }

    const char* p = args;
    if (p[0] == '-' && p[1] == '9') { p += 2; while (*p == ' ') p++; }

    int pid = 0;
    while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
    if (pid <= 0) { set_color(0x0C); print("kill: invalid pid\n"); set_color(0x07); tox_exit(); }

    if (tox_kill(pid) < 0) {
        set_color(0x0C); print("kill: no such process\n"); set_color(0x07);
    } else {
        set_color(0x0A); print("killed: "); print(args); print("\n"); set_color(0x07);
    }
    tox_exit();
}
