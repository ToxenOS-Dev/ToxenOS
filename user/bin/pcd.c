#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    print(args[0]?args:"/TxFS-1"); print("\n"); tox_exit();
}
