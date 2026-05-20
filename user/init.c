// ToxenOS/user/init.c — PID 1
// Sets up environment, shows login prompt, spawns shell on success.
#include "tox.h"

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int read_line(char* buf, int max, int hide) {
    int i = 0;
    while (i < max - 1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i] = 0; print("\n"); return i; }
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; tox_erase(); }
            continue;
        }
        // Ignore non-printable chars (scan codes, garbage from boot)
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++] = c;
        if (hide) print("*");
        else { char s[2] = {c, 0}; print(s); }
    }
    buf[i] = 0;
    return i;
}

// Check /etc/users for username:password match. Returns 1 if ok.
// File format: username:password\n  (one per line)
// If file missing, any username with empty password is accepted (default admin).
static int check_login(const char* user, const char* pass) {
    int size = tox_stat("/C:/etc/users");
    if (size <= 0) {
        // No users file: accept "admin" with empty password
        return str_eq(user, "admin") && pass[0] == 0;
    }

    char* buf = malloc((uint32_t)size + 1);
    if (!buf) return 0;
    int fd = tox_open("/C:/etc/users", 1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;

    const char* p = buf;
    while (*p) {
        char uname[64], pwd[64];
        int ui = 0, pi = 0;
        while (*p && *p != ':' && *p != '\n' && ui < 63) uname[ui++] = *p++;
        uname[ui] = 0;
        if (*p == ':') { p++;
            while (*p && *p != '\n' && pi < 63) pwd[pi++] = *p++;
        }
        pwd[pi] = 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        if (str_eq(uname, user) && str_eq(pwd, pass)) { free(buf); return 1; }
    }
    free(buf);
    return 0;
}

void _start() {
    // Set environment
    tox_setenv("PATH",  "/C:/BSM/SystemT:/C:/BSM/usr/lst");
    tox_setenv("HOME",  "/C:");
    tox_setenv("SHELL", "/shell.elf");
    tox_setenv("TERM",  "toxterm");
    tox_setenv("OS",    "ToxenOS");

    // Load persisted hostname from /etc/reg if present
    {
        int sz = tox_stat("/C:/etc/reg");
        if (sz > 0 && sz < 4096) {
            char* rb = malloc((uint32_t)sz + 1);
            if (rb) {
                int fd = tox_open("/C:/etc/reg", 1);
                int nn = tox_read(fd, (uint8_t*)rb, (uint32_t)sz);
                tox_close(fd);
                if (nn < 0) nn = 0;
                rb[nn] = 0;
                // Find hostname= line
                const char* p = rb;
                while (*p) {
                    if (p[0]=='h'&&p[1]=='o'&&p[2]=='s'&&p[3]=='t'&&
                        p[4]=='n'&&p[5]=='a'&&p[6]=='m'&&p[7]=='e'&&p[8]=='=') {
                        p += 9;
                        char hn[64]; int hi = 0;
                        while (*p && *p != '\n' && hi < 63) hn[hi++] = *p++;
                        hn[hi] = 0;
                        if (hi > 0) tox_setenv("hostname", hn);
                        break;
                    }
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                }
                free(rb);
            }
        }
    }

    // Drain keyboard buffer — boot process leaves garbage scan codes
    for (int _d = 0; _d < 500; _d++) yield();
    while (tox_keyavail()) tox_getchar();

    // Login prompt
    char username[64], password[64];
    while (1) {
        set_color(0x07); print("\n");
        set_color(0x0B); print("ToxenOS"); set_color(0x07); print(" login: ");
        read_line(username, sizeof(username), 0);

        set_color(0x07); print("Password: ");
        read_line(password, sizeof(password), 1);

        if (check_login(username, password)) {
            tox_setenv("USER", username);
            set_color(0x0A);
            print("\nWelcome, "); print(username); print("!\n");
            set_color(0x07);
            break;
        }

        set_color(0x0C); print("Login incorrect.\n"); set_color(0x07);
    }

    // Spawn shell — when it exits (logout), loop back to login prompt
    while (1) {
        int shell_pid = tox_spawn_embedded(0);
        tox_wait(shell_pid);
        // Shell exited — drain keyboard and show login again
        for (int _d = 0; _d < 300; _d++) yield();
        while (tox_keyavail()) tox_getchar();
    }
}
