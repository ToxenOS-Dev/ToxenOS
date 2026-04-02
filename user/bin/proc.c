#include "../tox.h"

static inline int tox_proc_list(uint8_t* buf, uint32_t size) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(31), "b"(buf), "c"(size));
    return r;
}

static void print_num(uint32_t n) {
    char buf[12]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i-1; j >= 0; j--) {
        char s[2] = {buf[j], 0}; print(s);
    }
}

void _start() {
    uint8_t buf[640]; // 16 processes * 40 bytes
    int len = tox_proc_list(buf, 640);
    if (len <= 0) {
        set_color(0x08); print("no processes\n"); set_color(0x07);
        tox_exit();
    }

    set_color(0x0E); print("PID  STATE    NAME\n");
    print("---  -------  ----------------\n");
    set_color(0x07);

    int count = len / 40;
    for (int i = 0; i < count; i++) {
        uint8_t* entry = buf + i * 40;
        uint32_t pid   = entry[0] | (entry[1] << 8);
        uint32_t state = entry[4];
        char* name     = (char*)(entry + 8);

        // PID
        set_color(0x0B);
        if (pid < 10) print(" ");
        if (pid < 100) print(" ");
        print_num(pid);
        print("  ");

        // State
        set_color(0x07);
        if (state == 0)      { set_color(0x0A); print("ready   "); }
        else if (state == 1) { set_color(0x0B); print("running "); }
        else                 { set_color(0x08); print("dead    "); }
        set_color(0x07);
        print(" ");

        // Name
        print(name); print("\n");
    }
    set_color(0x07);
    tox_exit();
}
