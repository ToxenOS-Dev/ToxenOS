#include "../tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int ends_with(const char* s, const char* suffix) {
    int sl = tox_strlen(s), el = tox_strlen(suffix);
    if (sl < el) return 0;
    return str_eq(s + sl - el, suffix);
}

void _start() {
    char args[256]; tox_get_args(args);

    if (str_eq(args, "clear")) {
        char name[128];
        for (int i = 0; ; i++) {
            if (tox_readdir("/C:/Trash", name, (uint32_t)i) < 0) break;
            if (name[0] == '.') continue;
            char path[256];
            tox_strcpy(path, "/C:/Trash/");
            tox_strcat(path, name);
            tox_remove(path);
        }
        set_color(0x0A); print("Trash cleared.\n"); set_color(0x07);
        tox_exit();
    }

    // List Trash contents (skip .origin metadata files)
    print("Trash (/C:/Trash/):\n");
    char name[128];
    int count = 0;
    for (int i = 0; ; i++) {
        if (tox_readdir("/C:/Trash", name, (uint32_t)i) < 0) break;
        if (name[0] == '.') continue;
        if (ends_with(name, ".origin")) continue;
        set_color(0x0E); print("  "); print(name); print("\n"); set_color(0x07);
        count++;
    }
    if (count == 0) { set_color(0x08); print("  (empty)\n"); set_color(0x07); }
    else { print("Use 'restore <name>' to recover, 'trash clear' to permanently delete.\n"); }
    tox_exit();
}
