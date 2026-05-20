#include "../tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int read_visible(char* buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i] = 0; print("\n"); return i; }
        if ((c == '\b' || c == 127) && i > 0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++] = c;
        char s[2] = {c, 0}; print(s);
    }
    buf[i] = 0; return i;
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
    // Drain keyboard buffer
    for (int _i = 0; _i < 100; _i++) yield();
    while (tox_keyavail()) tox_getchar();

    // Interactive — no arg needed, avoids cwd-prepend issues
    char username[64];
    set_color(0x07); print("New username: ");
    read_visible(username, sizeof(username));
    if (!username[0]) { set_color(0x0C); print("adduser: username cannot be empty\n"); set_color(0x07); tox_exit(); }

    // Check if user already exists
    int size = tox_stat("/C:/etc/users");
    char buf[2048]; buf[0] = 0;
    if (size > 0 && size < 2047) {
        int fd = tox_open("/C:/etc/users", 1);
        int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
        tox_close(fd); if (n < 0) n = 0; buf[n] = 0;
        const char* p = buf;
        while (*p) {
            char uname[64]; int ui = 0;
            while (*p && *p != ':' && *p != '\n' && ui < 63) uname[ui++] = *p++;
            uname[ui] = 0;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            if (str_eq(uname, username)) {
                set_color(0x0C); print("adduser: user already exists: "); print(username); print("\n");
                set_color(0x07); tox_exit();
            }
        }
    }

    char pass1[64], pass2[64];
    print("Password: ");       read_pass(pass1, sizeof(pass1));
    print("Confirm password: "); read_pass(pass2, sizeof(pass2));
    if (!str_eq(pass1, pass2)) {
        set_color(0x0C); print("adduser: passwords do not match\n"); set_color(0x07); tox_exit();
    }

    // Hash password with PBKDF2-SHA256
    set_color(0x08); print("Hashing password...\n"); set_color(0x07);
    char hashed[128];
    if (hash_password(pass1, hashed) < 0) {
        set_color(0x0C); print("adduser: failed to hash password\n"); set_color(0x07); tox_exit();
    }

    int fd = tox_open("/C:/etc/users", 2 | 4 | 8);
    if (fd < 0) { set_color(0x0C); print("adduser: cannot write /etc/users\n"); set_color(0x07); tox_exit(); }
    tox_write(fd, (uint8_t*)username, (uint32_t)tox_strlen(username));
    tox_write(fd, (uint8_t*)":", 1);
    tox_write(fd, (uint8_t*)hashed, (uint32_t)tox_strlen(hashed));
    tox_write(fd, (uint8_t*)":user\n", 6);
    tox_close(fd);

    set_color(0x0A); print("User created: "); print(username); print("\n"); set_color(0x07);
    tox_exit();
}
