#include "../tox.h"

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) { print("Usage: wc <file>\n"); tox_exit(); }

    int size = tox_stat(args);
    if (size < 0) {
        set_color(0x0C); print("wc: not found: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }

    char* buf = (char*)malloc((uint32_t)size + 1);
    if (!buf) { set_color(0x0C); print("wc: out of memory\n"); set_color(0x07); tox_exit(); }

    int fd = tox_open(args, 1);
    tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd);
    buf[size] = 0;

    int lines = 0, words = 0, bytes = size;
    int in_word = 0;

    for (int i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            in_word = 0;
        } else {
            if (!in_word) { words++; in_word = 1; }
        }
    }

    free(buf);

    set_color(0x0B); print_int(lines); set_color(0x08); print(" lines  ");
    set_color(0x0B); print_int(words); set_color(0x08); print(" words  ");
    set_color(0x0B); print_int(bytes); set_color(0x08); print(" bytes  ");
    set_color(0x07); print(args); print("\n");
    tox_exit();
}
