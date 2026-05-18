#include "../tox.h"

static int starts_with(const char* s, const char* p) {
    int i = 0; while (p[i] && s[i] == p[i]) i++; return p[i] == 0;
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int is_protected_dir(const char* p) {
    return str_eq(p, "/C:/Trash")  || str_eq(p, "/C:/Trash/")  ||
           str_eq(p, "/C:/BSM")    || str_eq(p, "/C:/BSM/")    ||
           str_eq(p, "/C:/etc")    || str_eq(p, "/C:/etc/");
}

static const char* basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) if (*p == '/') last = p + 1;
    return last;
}

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) { print("Usage: rm <file>\n"); tox_exit(); }

    if (starts_with(args, "/C:/BSM/SystemT/")) {
        set_color(0x0C);
        print("rm: permission denied -- /BSM/SystemT/ is a protected system directory.\n");
        print("    Only the ToxenOS system installer or update manager can modify it.\n");
        set_color(0x07); tox_exit();
    }

    if (is_protected_dir(args)) {
        set_color(0x0C);
        print("rm: cannot remove system directory: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }

    if (tox_isdir(args) == 1) {
        if (tox_remove(args) < 0) {
            set_color(0x0C); print("rm: failed to remove directory: "); print(args); print("\n");
            set_color(0x07);
        } else {
            set_color(0x0A); print("removed: "); print(args); print("\n"); set_color(0x07);
        }
        tox_exit();
    }

    int size = tox_stat(args);
    if (size < 0) {
        set_color(0x0C); print("rm: not found: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }

    const char* name = basename(args);
    char trash_path[256];
    tox_strcpy(trash_path, "/C:/Trash/");
    tox_strcat(trash_path, name);

    // Copy file to Trash
    uint8_t* buf = malloc((uint32_t)size);
    if (!buf) { set_color(0x0C); print("rm: out of memory\n"); set_color(0x07); tox_exit(); }

    int fd = tox_open(args, 1);
    tox_read(fd, buf, (uint32_t)size);
    tox_close(fd);

    fd = tox_open(trash_path, 2 | 4);
    if (fd < 0) {
        // Trash unavailable — fall back to permanent delete
        free(buf);
        if (tox_remove(args) < 0) {
            set_color(0x0C); print("rm: failed: "); print(args); print("\n"); set_color(0x07);
        } else {
            set_color(0x0A); print("removed (permanent): "); print(name); print("\n"); set_color(0x07);
        }
        tox_exit();
    }
    tox_write(fd, buf, (uint32_t)size);
    tox_close(fd);
    free(buf);

    // Save original path so restore knows where to put it back
    char origin_path[256];
    tox_strcpy(origin_path, trash_path);
    tox_strcat(origin_path, ".origin");
    fd = tox_open(origin_path, 2 | 4);
    if (fd >= 0) { tox_write(fd, (uint8_t*)args, (uint32_t)tox_strlen(args)); tox_close(fd); }

    tox_remove(args);
    set_color(0x0E); print("moved to Trash: "); print(name); print("\n"); set_color(0x07);
    tox_exit();
}
