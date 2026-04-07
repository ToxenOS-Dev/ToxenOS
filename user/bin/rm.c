#include "../tox.h"
void _start() {
    char args[256]; tox_get_args(args);
    if(!args[0]){print("Usage: rm <file>\n");tox_exit();}
    if(tox_remove(args)<0){set_color(0x0C);print("rm: failed\n");set_color(0x07);tox_exit();}
    set_color(0x0A);print("removed: ");print(args);print("\n");set_color(0x07);
    tox_exit();
}
