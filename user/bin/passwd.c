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
    // If no arg, get current user from env
    char current_user[64]; current_user[0] = 0;
    tox_getenv("USER", current_user, sizeof(current_user));
    if (!args[0]) {
        if (current_user[0]) tox_strcpy(args, current_user);
        else tox_strcpy(args, "admin");
    }

    // Changing another user's password requires admin
    if (!str_eq(args, current_user) && !tox_is_admin()) {
        set_color(0x0C); print("passwd: permission denied — admin required to change another user's password\n");
        set_color(0x07); tox_exit();
    }

    // Read new password
    print("New password for "); print(args); print(": ");
    char pass1[64]; read_pass(pass1, sizeof(pass1));
    print("Confirm password: ");
    char pass2[64]; read_pass(pass2, sizeof(pass2));
    if (!str_eq(pass1, pass2)) {
        set_color(0x0C); print("passwd: passwords do not match\n"); set_color(0x07); tox_exit();
    }

    // Hash with PBKDF2-SHA256
    set_color(0x08); print("Hashing password...\n"); set_color(0x07);
    char hashed[128];
    if (hash_password(pass1, hashed) < 0) {
        set_color(0x0C); print("passwd: failed to hash password\n"); set_color(0x07); tox_exit();
    }

    // Rewrite /etc/users replacing the matching line
    int size = tox_stat("/C:/etc/users");
    if (size <= 0) { set_color(0x0C); print("passwd: /etc/users not found\n"); set_color(0x07); tox_exit(); }

    char* buf = malloc((uint32_t)size + 1);
    if (!buf) { set_color(0x0C); print("passwd: out of memory\n"); set_color(0x07); tox_exit(); }
    int fd = tox_open("/C:/etc/users", 1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd); if (n < 0) n = 0; buf[n] = 0;

    char out[2048]; int oi = 0; int found = 0;
    const char* p = buf;
    while (*p) {
        char uname[64]; int ui = 0;
        const char* line_start = p;
        while (*p && *p != ':' && *p != '\n' && ui < 63) uname[ui++] = *p++;
        uname[ui] = 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        if (str_eq(uname, args)) {
            // Replace with new hashed password
            for (int i = 0; uname[i] && oi < 2046; i++) out[oi++] = uname[i];
            out[oi++] = ':';
            for (int i = 0; hashed[i] && oi < 2046; i++) out[oi++] = hashed[i];
            out[oi++] = '\n';
            found = 1;
        } else {
            int llen = (int)(p - line_start);
            for (int i = 0; i < llen && oi < 2046; i++) out[oi++] = line_start[i];
        }
    }
    out[oi] = 0;
    free(buf);

    if (!found) {
        set_color(0x0C); print("passwd: user not found: "); print(args); print("\n");
        print("  Tip: use 'adduser' to create a new user\n");
        set_color(0x07); tox_exit();
    }

    fd = tox_open("/C:/etc/users", 2 | 4);
    if (fd >= 0) { tox_write(fd, (uint8_t*)out, (uint32_t)oi); tox_close(fd); }
    set_color(0x0A); print("Password updated for: "); print(args); print("\n"); set_color(0x07);
    tox_exit();
}
