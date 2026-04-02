#include "../tox.h"

static inline int tox_bmsg(char* buf, uint32_t size) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(30), "b"(buf), "c"(size));
    return r;
}

void _start() {
    char buf[4096];
    int len = tox_bmsg(buf, 4096);
    if (len <= 0) {
        set_color(0x08); print("(no boot messages)\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x08); print("-- Boot messages --\n"); set_color(0x07);
    print(buf);
    tox_exit();
}
