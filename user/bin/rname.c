#include "../tox.h"

static int starts_with(const char* s, const char* p) {
    int i = 0; while (p[i] && s[i] == p[i]) i++; return p[i] == 0;
}

static void protected_err(void) {
    set_color(0x0C);
    print("rname: permission denied -- /BSM/SystemT/ is a protected system directory.\n");
    print("       Only the ToxenOS system installer or update manager can modify it.\n");
    set_color(0x07);
}

void _start() {
    char args[512];
    tox_get_args(args);

    char src[256], dst[256];
    int i = 0, j = 0;

    while (args[i] && args[i] != ' ') src[j++] = args[i++];
    src[j] = 0;
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i]) dst[j++] = args[i++];
    dst[j] = 0;

    if (!src[0] || !dst[0]) {
        set_color(0x0C); print("usage: rname <old> <new>\n");
        set_color(0x07); tox_exit();
    }

    if (starts_with(src, "/C:/BSM/SystemT/") || starts_with(dst, "/C:/BSM/SystemT/")) {
        protected_err(); tox_exit();
    }

    if (tox_stat(src) < 0) {
        set_color(0x0C); print("rname: not found: "); print(src); print("\n");
        set_color(0x07); tox_exit();
    }

    int size = tox_stat(src);
    uint8_t* buf = malloc((uint32_t)size);
    if (!buf) {
        set_color(0x0C); print("rname: out of memory\n");
        set_color(0x07); tox_exit();
    }

    int fd = tox_open(src, 1);
    tox_read(fd, buf, (uint32_t)size);
    tox_close(fd);

    fd = tox_open(dst, 2 | 4);
    if (fd < 0) {
        set_color(0x0C); print("rname: cannot create: "); print(dst); print("\n");
        free(buf); set_color(0x07); tox_exit();
    }
    tox_write(fd, buf, (uint32_t)size);
    tox_close(fd);

    tox_remove(src);
    free(buf);
    tox_exit();
}
