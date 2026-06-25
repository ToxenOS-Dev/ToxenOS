// ToxenOS/system_manager/system_tools/command_tools/where.c — Milestone
// 11: shows how shell64 would resolve a bare command name to a path
// (ToxenOS's own equivalent of "which"). Deliberately duplicates
// shell64.c's resolve_and_spawn probe order here -- no shared
// lookup syscall exists yet, so the two must be kept in sync by hand
// (a documented, accepted gap for this milestone). Uses sys_stat only,
// as a pure existence check -- never spawns the candidate, unlike
// shell64 itself.
#include <stdint.h>
#include "tox64.h"

#define CMDTOOLS_PATH "/system_manager/system_tools/command_tools/"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void str_cat(char* dst, const char* src, int max) {
    int i = my_strlen(dst);
    int j = 0;
    while (src[j] && i < max - 1) dst[i++] = src[j++];
    dst[i] = 0;
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

// Tests one candidate path for existence (sys_stat only -- never
// spawns). Returns 1 and leaves `path` as the match, or 0.
static int try_path(char* path, int max, const char* dir, const char* cmd, const char* ext) {
    str_copy(path, dir, max);
    str_cat(path, cmd, max);
    str_cat(path, ext, max);
    uint64_t size;
    return sys_stat(path, &size, 0) == 0;
}

void _start(void) {
    char args[128];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    if (!args[0]) {
        put("where: missing argument\n");
        sys_exit(1);
    }

    if (args[0] == '/') {
        uint64_t size;
        if (sys_stat(args, &size, 0) == 0) { put(args); put("\n"); sys_exit(0); }
        put("where: not found: "); put(args); put("\n");
        sys_exit(1);
    }

    char path[128];
    if (try_path(path, sizeof(path), CMDTOOLS_PATH, args, ".nex64") ||
        try_path(path, sizeof(path), CMDTOOLS_PATH, args, ".elf64") ||
        try_path(path, sizeof(path), "/",            args, ".nex64") ||
        try_path(path, sizeof(path), "/",            args, ".elf64")) {
        put(path);
        put("\n");
        sys_exit(0);
    }

    put("where: not found: "); put(args); put("\n");
    sys_exit(1);

    for (;;) { }  // unreachable
}
