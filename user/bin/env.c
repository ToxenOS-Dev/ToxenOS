// user/bin/env.c — list kernel environment variables
#include "../tox.h"

void _start() {
    char key[32], val[128];
    for (int i = 0; tox_envlist(i, key, val) == 0; i++) {
        print(key); print("="); print(val); print("\n");
    }
    tox_exit();
}
