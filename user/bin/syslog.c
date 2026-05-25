#include "../tox.h"

static int starts_with(const char* s, const char* p) {
    int i = 0; while (p[i] && s[i] == p[i]) i++; return p[i] == 0;
}

void _start() {
    char* buf = malloc(65536);
    if (!buf) { print("syslog: out of memory\n"); tox_exit(); }

    int n = tox_bmsg(buf, 65536);
    if (n <= 0) {
        set_color(0x08); print("syslog: (empty)\n"); set_color(0x07);
        free(buf); tox_exit();
    }
    buf[n] = 0;

    set_color(0x08); print("=== ToxenOS System Log ===\n"); set_color(0x07);

    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = 0;

        if (starts_with(line, "ERROR") || starts_with(line, "PANIC") || starts_with(line, "FAULT"))
            set_color(0x0C);
        else if (starts_with(line, "WARN"))
            set_color(0x0E);
        else if (starts_with(line, "Mounted") || starts_with(line, "TxFS") || starts_with(line, "ATA"))
            set_color(0x0A);
        else
            set_color(0x07);

        print(line); print("\n");
    }
    set_color(0x07);
    free(buf);
    tox_exit();
}
