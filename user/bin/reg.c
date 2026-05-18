#include "../tox.h"

#define REG_PATH "/C:/etc/reg"
#define REG_SIZE 4096

static int seq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; } return *a == *b;
}

// Read entire reg file into buf (zero-terminated), return length
static int reg_read(char* buf) {
    int fd = tox_open(REG_PATH, 1);
    if (fd < 0) { buf[0] = 0; return 0; }
    int n = tox_read(fd, (uint8_t*)buf, REG_SIZE - 1);
    tox_close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

static void reg_write(const char* buf) {
    int fd = tox_open(REG_PATH, 2 | 4);
    if (fd < 0) return;
    tox_write(fd, (const uint8_t*)buf, (uint32_t)tox_strlen(buf));
    tox_close(fd);
}

// Find value for key in buf, copy to out. Returns 1 if found.
static int reg_find(const char* buf, const char* key, char* out) {
    int klen = tox_strlen(key);
    const char* p = buf;
    while (*p) {
        if (tox_starts_with(p, key) && p[klen] == '=') {
            const char* v = p + klen + 1;
            int i = 0;
            while (v[i] && v[i] != '\n' && i < 127) { out[i] = v[i]; i++; }
            out[i] = 0;
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

// Rebuild buf without key (for del/set). Returns new length.
static int reg_remove(char* buf, const char* key) {
    int klen = tox_strlen(key);
    char tmp[REG_SIZE]; int ti = 0;
    const char* p = buf;
    while (*p) {
        const char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (tox_starts_with(line, key) && line[klen] == '=') continue;
        int llen = (int)(p - line);
        for (int i = 0; i < llen && ti < REG_SIZE-1; i++) tmp[ti++] = line[i];
    }
    tmp[ti] = 0;
    tox_strcpy(buf, tmp);
    return ti;
}

void _start() {
    char args[512]; tox_get_args(args);
    if (!args[0]) {
        print("Usage: reg set <key> <value>\n");
        print("       reg get <key>\n");
        print("       reg del <key>\n");
        print("       reg list\n");
        tox_exit();
    }

    char cmd[16]; int ci = 0;
    while (args[ci] && args[ci] != ' ' && ci < 15) { cmd[ci] = args[ci]; ci++; }
    cmd[ci] = 0;
    while (args[ci] == ' ') ci++;
    char key[64]; int ki = 0;
    while (args[ci] && args[ci] != ' ' && ki < 63) { key[ki++] = args[ci++]; }
    key[ki] = 0;
    while (args[ci] == ' ') ci++;
    const char* val = args + ci;

    char buf[REG_SIZE];
    reg_read(buf);

    if (seq(cmd, "list")) {
        set_color(0x0B); print("Registry (/etc/reg):\n"); set_color(0x07);
        int count = 0;
        const char* p = buf;
        while (*p) {
            const char* line = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            if ((int)(p - line) > 1) {
                set_color(0x0A); print("  ");
                const char* q = line;
                while (q < p && *q != '\n') { char c[2]={*q++,0}; print(c); }
                print("\n"); set_color(0x07);
                count++;
            }
        }
        if (!count) { set_color(0x08); print("  (empty)\n"); set_color(0x07); }
        tox_exit();
    }

    if (seq(cmd, "get")) {
        char out[128];
        if (!reg_find(buf, key, out)) {
            set_color(0x0C); print("reg: not found: "); print(key); print("\n"); set_color(0x07);
        } else {
            set_color(0x0B); print(key); set_color(0x08); print(" = ");
            set_color(0x07); print(out); print("\n");
        }
        tox_exit();
    }

    if (seq(cmd, "del")) {
        reg_remove(buf, key);
        reg_write(buf);
        set_color(0x0A); print("deleted: "); print(key); print("\n"); set_color(0x07);
        tox_exit();
    }

    if (seq(cmd, "set")) {
        if (!key[0] || !val[0]) {
            set_color(0x0C); print("reg: usage: reg set <key> <value>\n"); set_color(0x07);
            tox_exit();
        }
        reg_remove(buf, key);
        // Append KEY=VALUE\n
        tox_strcat(buf, key);
        tox_strcat(buf, "=");
        tox_strcat(buf, val);
        tox_strcat(buf, "\n");
        reg_write(buf);
        set_color(0x0A); print("set: "); print(key); print(" = "); print(val); print("\n");
        set_color(0x07);
        tox_exit();
    }

    set_color(0x0C); print("reg: unknown command: "); print(cmd); print("\n"); set_color(0x07);
    tox_exit();
}
