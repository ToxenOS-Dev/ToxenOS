// user/bin/alias.c — print saved aliases (reads /C:/etc/aliases)
// Enables: alias | sif pattern
#include "../tox.h"

void _start() {
    int size = tox_stat("/C:/etc/aliases");
    if (size <= 0) {
        tox_exit();
    }

    char* buf = malloc((uint32_t)size + 1);
    if (!buf) { set_color(0x0C); print("alias: out of memory\n"); set_color(0x07); tox_exit(); }

    int fd = tox_open("/C:/etc/aliases", 1);
    if (fd < 0) { free(buf); tox_exit(); }
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;

    print(buf);
    free(buf);
    tox_exit();
}
