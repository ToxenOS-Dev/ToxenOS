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
    char args[128]; tox_get_args(args);

    // If args is empty or just a drive path (e.g. "/C:"), show all
    // Otherwise pass raw args — kernel strips the path prefix itself
    if (!args[0] || (args[0] == '/' && !args[1])) {
        show("version"); show("hostname"); show("uptime"); show("procs");
        tox_exit();
    }

    // Single key: let kernel do the matching (it strips /C:/ prefix)
    char buf[128];
    if (tox_sysctl(args, buf, sizeof(buf)) < 0) {
        // Unknown — show all
        show("version"); show("hostname"); show("uptime"); show("procs");
    } else {
        // Find which key matched to print the label
        const char* key = args;
        for (const char* p = args; *p; p++) if (*p == '/') key = p + 1;
        set_color(0x0B); print(key); set_color(0x08); print(" = ");
        set_color(0x07); print(buf); print("\n");
    }
    tox_exit();
}
