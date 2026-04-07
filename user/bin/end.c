#include "../tox.h"

static int str_to_int(const char* s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

void _start() {
    char args[64];
    tox_get_args(args);

    if (!args[0]) {
        set_color(0x0C); print("usage: end <pid>\n");
        set_color(0x07); tox_exit();
    }

    int pid = str_to_int(args);
    if (pid <= 0) {
        set_color(0x0C); print("end: invalid pid\n");
        set_color(0x07); tox_exit();
    }

    if (tox_kill(pid) == 0) {
        set_color(0x0A); print("process "); 
        print(args); print(" terminated\n");
        set_color(0x07);
    } else {
        set_color(0x0C); print("end: no such process: ");
        print(args); print("\n");
        set_color(0x07);
    }
    tox_exit();
}
