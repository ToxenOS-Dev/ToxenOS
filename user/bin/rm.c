#include "../tox.h"

static int starts_with(const char* s, const char* p) {
    int i = 0;
    while (p[i] && s[i] == p[i]) i++;
    return p[i] == 0;
}

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) { print("Usage: rm <file>\n"); tox_exit(); }
    if (tox_remove(args) < 0) {
        set_color(0x0C);
        if (starts_with(args, "/BSM/SystemT/")) {
            print("rm: permission denied -- /BSM/SystemT/ is a protected system directory.\n");
            print("    Only the ToxenOS system installer or update manager can modify it.\n");
        } else {
            print("rm: failed: "); print(args); print("\n");
        }
        set_color(0x07);
        tox_exit();
    }
    set_color(0x0A); print("removed: "); print(args); print("\n"); set_color(0x07);
    tox_exit();
}
