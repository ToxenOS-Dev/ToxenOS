#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    if (!args[0]) { print("Usage: shw <file>\n"); exit(); }
    int fd=open(args, 1);
    if (fd<0) { set_color(0x0C); print("shw: not found\n"); set_color(0x07); exit(); }
    uint8_t buf[256]; int n;
    while ((n=read(fd,buf,255))>0) { buf[n]=0; print((char*)buf); }
    close(fd); print("\n"); exit();
}
