#include "../tox.h"

static inline int tox_proc_list(uint8_t* buf, uint32_t size) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(31), "b"(buf), "c"(size));
    return r;
}
static inline int key_available() {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(29)); return r;
}
static inline char getchar() {
    int r; __asm__ volatile("int $0x80":"=a"(r):"a"(2)); return (char)r;
}

static void print_num(uint32_t n) {
    char buf[12]; int i = 0;
    if (n == 0) { print("0"); return; }
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    for (int j = i-1; j >= 0; j--) { char s[2]={buf[j],0}; print(s); }
}

void _start() {
    while (1) {
        tox_clear();
        set_color(0x0E);
        print("ToxenOS - top  (press q to quit)\n");
        print("PID  STATE    NAME\n");
        print("---  -------  ----------------\n");
        set_color(0x07);

        uint8_t buf[640];
        int len = tox_proc_list(buf, 640);
        int count = len > 0 ? len / 40 : 0;

        for (int i = 0; i < count; i++) {
            uint8_t* entry = buf + i * 40;
            uint32_t pid   = entry[0] | (entry[1] << 8);
            uint32_t state = entry[4];
            char* name     = (char*)(entry + 8);

            set_color(0x0B);
            if (pid < 10) print(" ");
            if (pid < 100) print(" ");
            print_num(pid);
            print("  ");

            if (state == 0)      { set_color(0x0A); print("ready   "); }
            else if (state == 1) { set_color(0x0B); print("running "); }
            else                 { set_color(0x08); print("dead    "); }
            set_color(0x07);
            print(" "); print(name); print("\n");
        }

        // Wait ~1 second then refresh, or quit on 'q'
        for (int i = 0; i < 5000000; i++) {
            if (key_available()) {
                char c = getchar();
                if (c == 'q' || c == 'Q') {
                    tox_clear(); tox_exit();
                }
            }
        }
    }
}
