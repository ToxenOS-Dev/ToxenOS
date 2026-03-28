#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    if(!args[0]){print("Usage: mkef <file>\n");tox_exit();}
    int fd=tox_open(args,1|4);
    if(fd<0){set_color(0x0C);print("mkef: failed\n");set_color(0x07);tox_exit();}
    tox_close(fd);
    set_color(0x0A);print("created: ");print(args);print("\n");set_color(0x07);
    tox_exit();
}
