#include "../tox.h"

void _start() {
    char buf[32];
    if (tox_gettime(buf, sizeof(buf)) < 0) {
        set_color(0x0C); print("date: failed to read clock\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x0B); print(buf); set_color(0x07); print("\n");
    tox_exit();
}
