// user/bin/touch.c — create empty file or update existing (no-op on existing)
#include "../tox.h"

void _start() {
    char args[256]; tox_get_args(args);
    char* p = args;
    while (*p == ' ') p++;

    if (!*p) {
        print("Usage: touch <file> [file2 ...]\n");
        tox_exit();
    }

    while (*p) {
        char path[128]; int i = 0;
        while (*p && *p != ' ' && i < 127) path[i++] = *p++;
        path[i] = 0;
        while (*p == ' ') p++;
        if (!path[0]) continue;

        if (tox_stat(path) >= 0) continue; // already exists

        int fd = tox_open(path, 2 | 4); // create
        if (fd < 0) {
            set_color(0x0C); print("touch: cannot create: "); print(path); print("\n");
            set_color(0x07);
        } else {
            tox_close(fd);
        }
    }

    tox_exit();
}
