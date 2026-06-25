// ToxenOS/system_manager/system_tools/command_tools/stat.c — Milestone
// 11: ToxenOS64's first real "stat" command (ToxenOS never had one on
// the 32-bit side either -- tox_stat() was always just a syscall
// wrapper). Prints a file's size and type using the Milestone 11
// type-aware sys_stat.
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

static void put_uint(uint64_t v) {
    char rev[24];
    int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    char out[24];
    int n = 0;
    while (rn > 0) out[n++] = rev[--rn];
    out[n] = 0;
    put(out);
}

void _start(void) {
    char args[128];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    if (!args[0]) {
        put("stat: missing argument\n");
        sys_exit(1);
    }

    uint64_t size = 0;
    int is_dir = 0;
    if (sys_stat(args, &size, &is_dir) < 0) {
        put("stat: cannot stat "); put(args); put("\n");
        sys_exit(1);
    }

    put(args);
    put(": ");
    put_uint(size);
    put(is_dir ? " bytes, directory\n" : " bytes, file\n");
    sys_exit(0);

    for (;;) { }  // unreachable
}
