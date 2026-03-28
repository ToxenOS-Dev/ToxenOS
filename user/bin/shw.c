#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    if(!args[0]){print("Usage: shw <file>\n");tox_exit();}
    int fd=tox_open(args,1);
    if(fd<0){set_color(0x0C);print("shw: not found\n");set_color(0x07);tox_exit();}
    uint8_t buf[256]; int n;
    while((n=tox_read(fd,buf,255))>0){buf[n]=0;print((char*)buf);}
    tox_close(fd); print("\n"); tox_exit();
}
