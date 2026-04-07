#include "../tox.h"
void _start() {
    char args[256]; tox_get_args(args);
    print(args); print("\n"); tox_exit();
}
