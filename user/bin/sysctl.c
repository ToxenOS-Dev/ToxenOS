#include "../tox.h"

static void show(const char* key) {
    char buf[128];
    if (tox_sysctl(key, buf, sizeof(buf)) < 0) return;
    set_color(0x0B); print(key); set_color(0x08); print(" = ");
    set_color(0x07); print(buf); print("\n");
}

void _start() {
    show("version");
    show("hostname");
    show("uptime");
    show("procs");
    tox_exit();
}
