#include "../tox.h"

static int starts_with(const char* s, const char* p) {
    int i = 0; while (p[i] && s[i] == p[i]) i++; return p[i] == 0;
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int is_protected_dir(const char* p) {
    return str_eq(p, "/C:/BSM") || str_eq(p, "/C:/BSM/") ||
           str_eq(p, "/C:/etc") || str_eq(p, "/C:/etc/");
}

static void rm_one(const char* args);

void _start() {
    char args[512]; tox_get_args(args);
    if (!args[0]) { print("Usage: rm <file> [file2 ...]\n"); tox_exit(); }

    // Support multiple space-separated paths (wildcard expansion)
    const char* p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char path[256]; int pi = 0;
        while (*p && *p != ' ' && pi < 255) path[pi++] = *p++;
        path[pi] = 0;
        rm_one(path);
    }
    tox_exit();
}

static void rm_one(const char* args) {

    if (starts_with(args, "/C:/BSM/SystemT/")) {
        set_color(0x0C);
        print("rm: permission denied -- /BSM/SystemT/ is a protected system directory.\n");
        print("    Only the ToxenOS system installer or update manager can modify it.\n");
        set_color(0x07); return;
    }

    if (is_protected_dir(args)) {
        set_color(0x0C);
        print("rm: cannot remove system directory: "); print(args); print("\n");
        set_color(0x07); return;
    }

    if (tox_isdir(args) == 1) {
        if (tox_remove(args) < 0) {
            set_color(0x0C); print("rm: failed to remove directory: "); print(args); print("\n");
            set_color(0x07);
        } else {
            set_color(0x0A); print("removed: "); print(args); print("\n"); set_color(0x07);
        }
        return;
    }

    if (tox_stat(args) < 0) {
        set_color(0x0C); print("rm: not found: "); print(args); print("\n");
        set_color(0x07); return;
    }

    if (tox_remove(args) < 0) {
        set_color(0x0C); print("rm: failed: "); print(args); print("\n"); set_color(0x07);
    } else {
        set_color(0x0A); print("removed: "); print(args); print("\n"); set_color(0x07);
    }
}
