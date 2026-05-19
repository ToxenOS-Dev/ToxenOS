#include "../tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int read_pass(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i] = 0; print("\n"); return i; }
        if ((c == '\b' || c == 127) && i > 0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++] = c; print("*");
    }
    buf[i] = 0; return i;
}

void _start() {
    char args[64]; tox_get_args(args);
    if (!args[0]) { print("Usage: adduser <username>\n"); tox_exit(); }

    // Check if user already exists
    int size = tox_stat("/C:/etc/users");
    char buf[2048]; buf[0] = 0;
    if (size > 0 && size < 2047) {
        int fd = tox_open("/C:/etc/users", 1);
        int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
        tox_close(fd); if (n < 0) n = 0; buf[n] = 0;
        // Check if username exists
        const char* p = buf;
        while (*p) {
            char uname[64]; int ui = 0;
            while (*p && *p != ':' && *p != '\n' && ui < 63) uname[ui++] = *p++;
            uname[ui] = 0;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            if (str_eq(uname, args)) {
                set_color(0x0C); print("adduser: user already exists: "); print(args); print("\n");
                set_color(0x07); tox_exit();
            }
        }
    }

    // Get password
    char pass1[64], pass2[64];
    print("New password for "); print(args); print(": ");
    read_pass(pass1, sizeof(pass1));
    print("Confirm password: ");
    read_pass(pass2, sizeof(pass2));

    if (!str_eq(pass1, pass2)) {
        set_color(0x0C); print("adduser: passwords do not match\n"); set_color(0x07); tox_exit();
    }

    // Append to /etc/users
    int fd = tox_open("/C:/etc/users", 2 | 4 | 8);  // write+create+append
    if (fd < 0) {
        set_color(0x0C); print("adduser: cannot write /etc/users\n"); set_color(0x07); tox_exit();
    }
    tox_write(fd, (uint8_t*)args,  (uint32_t)tox_strlen(args));
    tox_write(fd, (uint8_t*)":",   1);
    tox_write(fd, (uint8_t*)pass1, (uint32_t)tox_strlen(pass1));
    tox_write(fd, (uint8_t*)"\n",  1);
    tox_close(fd);

    set_color(0x0A); print("User created: "); print(args); print("\n"); set_color(0x07);
    tox_exit();
}
