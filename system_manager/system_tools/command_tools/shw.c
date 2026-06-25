// ToxenOS/system_manager/system_tools/command_tools/shw.c — Milestone 11:
// ToxenOS64's file-display command. ToxenOS's own name for this
// (confirmed via the 32-bit user/bin/shw.c) -- deliberately not "cat".
// Runs as a standalone NEX64 process, spawned by shell64; retrieves its
// one argument (the file path) via sys_get_args, exactly mirroring the
// 32-bit tox_get_args shape.
#include <stdint.h>
#include "tox64.h"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

void _start(void) {
    char args[128];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    if (!args[0]) {
        put("shw: missing argument\n");
        sys_exit(1);
    }

    int64_t fd = sys_open(args);
    if (fd < 0) {
        put("shw: cannot open "); put(args); put("\n");
        sys_exit(1);
    }

    char buf[256];
    for (;;) {
        int64_t got = sys_read((int)fd, buf, sizeof(buf) - 1);
        if (got <= 0) break;
        buf[got] = 0;
        put(buf);
    }
    sys_close((int)fd);
    sys_exit(0);

    for (;;) { }  // unreachable
}
