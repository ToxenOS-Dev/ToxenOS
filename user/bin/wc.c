#include "../tox.h"

static void wc_one(const char* path) {
    int size = tox_stat(path);
    if (size < 0) {
        set_color(0x0C); print("wc: not found: "); print(path); print("\n");
        set_color(0x07); return;
    }

    char* buf = (char*)malloc((uint32_t)size + 1);
    if (!buf) { set_color(0x0C); print("wc: out of memory\n"); set_color(0x07); return; }

    int fd = tox_open(path, 1);
    tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd);
    buf[size] = 0;

    int lines = 0, words = 0, bytes = size, in_word = 0;
    for (int i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') in_word = 0;
        else if (!in_word) { words++; in_word = 1; }
    }
    free(buf);

    // basename for display
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/') name = p+1;

    set_color(0x0B); print_int(lines); set_color(0x08); print(" lines  ");
    set_color(0x0B); print_int(words); set_color(0x08); print(" words  ");
    set_color(0x0B); print_int(bytes); set_color(0x08); print(" bytes  ");
    set_color(0x07); print(name); print("\n");
}

void _start() {
    char args[512]; tox_get_args(args);
    if (!args[0]) { print("Usage: wc <file> [file2 ...]\n"); tox_exit(); }

    const char* p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char path[256]; int pi = 0;
        while (*p && *p != ' ' && pi < 255) path[pi++] = *p++;
        path[pi] = 0;
        wc_one(path);
    }
    tox_exit();
}
