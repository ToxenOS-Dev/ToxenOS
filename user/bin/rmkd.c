#include "../tox.h"

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) { print("Usage: rmkd <dir>\n"); tox_exit(); }
    if (tox_isdir(args) != 1) {
        set_color(0x0C); print("rmkd: not a directory: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }
    if (tox_remove(args) < 0) {
        set_color(0x0C); print("rmkd: failed: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }
    set_color(0x0A); print("removed: "); print(args); print("\n"); set_color(0x07);
    tox_exit();
}
