// ToxenOS/user/init.c — PID 1
// Sets up environment, shows user picker + login prompt, spawns shell.
#include "tox.h"

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
        buf[i++] = c; char s[2]={c,0}; print(s);
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

// Parse /etc/users, store usernames in names[]. Returns count.
#define MAX_USERS 16
static char user_names[MAX_USERS][64];
static char user_pwds[MAX_USERS][64];
static int  user_count = 0;

static void load_users(void) {
    user_count = 0;
    int size = tox_stat("/C:/etc/users");
    if (size <= 0 || size > 4095) {
        tox_strcpy(user_names[0], "admin");
        tox_strcpy(user_pwds[0], "");
        user_count = 1;
        return;
    }
    char* buf = malloc((uint32_t)size + 1);
    if (!buf) return;
    int fd = tox_open("/C:/etc/users", 1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd); if (n < 0) n = 0; buf[n] = 0;
    const char* p = buf;
    while (*p && user_count < MAX_USERS) {
        int ui = 0, pi = 0;
        while (*p && *p != ':' && *p != '\n' && ui < 63) user_names[user_count][ui++] = *p++;
        user_names[user_count][ui] = 0;
        if (*p == ':') { p++;
            while (*p && *p != '\n' && pi < 63) user_pwds[user_count][pi++] = *p++;
        }
        user_pwds[user_count][pi] = 0;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (ui > 0) user_count++;
    }
    free(buf);
    if (user_count == 0) {
        tox_strcpy(user_names[0], "admin");
        tox_strcpy(user_pwds[0], "");
        user_count = 1;
    }
}

// Print n spaces
static void spaces(int n) { while (n-- > 0) print(" "); }

static void show_user_screen(void) {
    tox_clear();

    // Terminal: 128 cols x 48 rows (1024x768, 8x16 font)
    // Vertical: top padding to center content (~10 lines) = ~19 newlines
    for (int i = 0; i < 17; i++) print("\n");

    // "==== Welcome to ToxenOS ====" = 30 chars, center in 128: pad 49
    spaces(49);
    set_color(0x06); print("==== ");
    set_color(0x0C); print("Welcome to ToxenOS");
    set_color(0x06); print(" ====");
    set_color(0x07); print("\n\n\n");

    load_users();

    // Center user list (approx col 55)
    for (int i = 0; i < user_count; i++) {
        spaces(55);
        set_color(0x08); print("[");
        set_color(0x0A); char n[2]={'0'+(char)(i+1),0}; print(n);
        set_color(0x08); print("]  ");
        set_color(0x07); print(user_names[i]); print("\n");
    }
    print("\n\n");

    // Center "Login as:" prompt (col 52)
    spaces(52);
    set_color(0x07);
}

static void drain_keyboard(void) {
    for (int _d = 0; _d < 1000; _d++) yield();
    while (tox_keyavail()) tox_getchar();
    for (int _d = 0; _d < 200; _d++) yield();
    while (tox_keyavail()) tox_getchar();
}

void _start() {
    // Set environment
    tox_setenv("PATH",  "/C:/BSM/SystemT:/C:/BSM/usr/lst");
    tox_setenv("HOME",  "/C:");
    tox_setenv("SHELL", "/shell.elf");
    tox_setenv("TERM",  "toxterm");
    tox_setenv("OS",    "ToxenOS");

    // Load persisted hostname from /etc/reg
    {
        int sz = tox_stat("/C:/etc/reg");
        if (sz > 0 && sz < 4096) {
            char* rb = malloc((uint32_t)sz + 1);
            if (rb) {
                int fd = tox_open("/C:/etc/reg", 1);
                int nn = tox_read(fd, (uint8_t*)rb, (uint32_t)sz);
                tox_close(fd); if (nn < 0) nn = 0; rb[nn] = 0;
                const char* p = rb;
                while (*p) {
                    if (p[0]=='h'&&p[1]=='o'&&p[2]=='s'&&p[3]=='t'&&
                        p[4]=='n'&&p[5]=='a'&&p[6]=='m'&&p[7]=='e'&&p[8]=='=') {
                        p += 9; char hn[64]; int hi = 0;
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

    drain_keyboard();

    // Login loop
    while (1) {
        show_user_screen();

        char username[64], password[64];

        // User selection: accept number or direct username
        print("Login as: ");
        read_visible(username, sizeof(username));
        if (!username[0]) continue;

        // If user typed a number, resolve to username
        if (username[0] >= '1' && username[0] <= '9' && username[1] == 0) {
            int idx = username[0] - '1';
            if (idx < user_count) tox_strcpy(username, user_names[idx]);
            else { set_color(0x0C); print("  Invalid selection.\n"); set_color(0x07); continue; }
        }

        spaces(52); print("Password: ");
        read_pass(password, sizeof(password));

        // Check credentials
        int ok = 0;
        for (int i = 0; i < user_count; i++) {
            if (str_eq(user_names[i], username) && str_eq(user_pwds[i], password)) {
                ok = 1; break;
            }
        }

        if (!ok) {
            spaces(52);
            set_color(0x0C); print("Login incorrect.\n"); set_color(0x07);
            for (int _d = 0; _d < 300; _d++) yield();
            continue;
        }

        tox_setenv("USER", username);
        spaces(52);
        set_color(0x0A); print("Welcome, "); print(username); print("!\n\n");
        set_color(0x07);

        // Spawn shell — when it exits (logout), show user screen again
        int shell_pid = tox_spawn_embedded(0);
        tox_wait(shell_pid);
        drain_keyboard();
    }
}
