#include "../tox.h"

void _start() {
    char* buf = malloc(65536);
    if (!buf) { print("bmsg: out of memory\n"); tox_exit(); }
    int len = tox_bmsg(buf, 65536);
    if (len <= 0) {
        set_color(0x08); print("(no boot messages)\n"); set_color(0x07);
        free(buf); tox_exit();
    }
    set_color(0x08); print("-- Boot messages --\n"); set_color(0x07);
    print(buf);
    free(buf);
    tox_exit();
}
