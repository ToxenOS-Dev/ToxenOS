#include "../tox.h"

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
