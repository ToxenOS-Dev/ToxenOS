#include "../tox.h"
void _start() {
    char args[256]; tox_get_args(args);
    if(!args[0]){print("Usage: mkd <dir>\n");tox_exit();}
    if(tox_mkdir(args)<0){set_color(0x0C);print("mkd: failed\n");set_color(0x07);tox_exit();}
    set_color(0x0A);print("created: ");print(args);print("\n");set_color(0x07);
    tox_exit();
}
