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

// Returns 1 if args contains the keyword as the last path component
static int has_key(const char* args, const char* key) {
    int al = tox_strlen(args), kl = tox_strlen(key);
    if (al < kl) return 0;
    // Check if args ends with /key or equals key exactly
    if (tox_strcmp(args + al - kl, key) != 0) return 0;
    if (al == kl) return 1;                      // exact match
    return args[al - kl - 1] == '/';             // preceded by /
}

void _start() {
    char args[128]; tox_get_args(args);

    if (has_key(args, "version"))  { show("version");  tox_exit(); }
    if (has_key(args, "hostname")) { show("hostname"); tox_exit(); }
    if (has_key(args, "uptime"))   { show("uptime");   tox_exit(); }
    if (has_key(args, "procs"))    { show("procs");    tox_exit(); }

    // No specific key — show all
    show("version");
    show("hostname");
    show("uptime");
    show("procs");
    tox_exit();
}
