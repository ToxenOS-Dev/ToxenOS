#include "../tox.h"

static void show(const char* key) {
    char buf[128];
    if (tox_sysctl(key, buf, sizeof(buf)) < 0) {
        set_color(0x0C); print("sysctl: unknown key: "); print(key); print("\n");
        set_color(0x07); return;
    }
    set_color(0x0B); print(key); set_color(0x08); print(" = ");
    set_color(0x07); print(buf); print("\n");
}

void _start() {
    char args[64]; tox_get_args(args);
    if (!args[0]) {
        show("version");
        show("hostname");
        show("uptime");
        show("procs");
        tox_exit();
    }
    show(args);
    tox_exit();
}
