#include "../tox.h"

static const char* basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) if (*p == '/') last = p + 1;
    return last;
}

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) { print("Usage: restore <filename>\n"); tox_exit(); }

    const char* name = basename(args);

    char trash_path[256];
    tox_strcpy(trash_path, "/C:/Trash/");
    tox_strcat(trash_path, name);

    char origin_path[256];
    tox_strcpy(origin_path, trash_path);
    tox_strcat(origin_path, ".origin");

    // Read original path
    int origin_size = tox_stat(origin_path);
    if (origin_size < 0) {
        set_color(0x0C); print("restore: not in Trash: "); print(name); print("\n");
        set_color(0x07); tox_exit();
    }

    char orig[256] = {0};
    int fd = tox_open(origin_path, 1);
    int n = tox_read(fd, (uint8_t*)orig, 255);
    tox_close(fd);
    orig[n] = 0;

    // Read file from Trash
    int size = tox_stat(trash_path);
    if (size < 0) {
        set_color(0x0C); print("restore: missing from Trash: "); print(name); print("\n");
        set_color(0x07); tox_exit();
    }

    uint8_t* buf = malloc((uint32_t)size);
    if (!buf) { set_color(0x0C); print("restore: out of memory\n"); set_color(0x07); tox_exit(); }

    fd = tox_open(trash_path, 1);
    tox_read(fd, buf, (uint32_t)size);
    tox_close(fd);

    fd = tox_open(orig, 2 | 4);
    if (fd < 0) {
        free(buf);
        set_color(0x0C); print("restore: cannot restore to: "); print(orig); print("\n");
        set_color(0x07); tox_exit();
    }
    tox_write(fd, buf, (uint32_t)size);
    tox_close(fd);
    free(buf);

    tox_remove(trash_path);
    tox_remove(origin_path);

    set_color(0x0A); print("restored: "); print(name); print(" -> "); print(orig); print("\n");
    set_color(0x07);
    tox_exit();
}
