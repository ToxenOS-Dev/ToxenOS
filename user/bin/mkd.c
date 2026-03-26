#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    if (!args[0]) { print("Usage: mkd <dir>\n"); exit(); }
    if (mkdir(args)<0) { set_color(0x0C); print("mkd: failed\n"); set_color(0x07); exit(); }
    set_color(0x0A); print("created: "); print(args); print("\n"); set_color(0x07);
    exit();
}
