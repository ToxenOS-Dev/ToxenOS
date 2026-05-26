// ToxenOS/user/init.c — PID 1
// Sets up environment and spawns the shell directly.
#include "tox.h"

void _start() {
    tox_setenv("PATH",  "/C:/BSM/SystemT:/C:/BSM/usr/lst");
    tox_setenv("HOME",  "/C:");
    tox_setenv("SHELL", "/shell.elf");
    tox_setenv("TERM",  "toxterm");
    tox_setenv("OS",    "ToxenOS");
    tox_setenv("USER",  "toxenos");

    // Load persisted hostname
    {
        int sz = tox_stat("/C:/etc/reg");
        if (sz > 0 && sz < 4096) {
            char* rb = malloc((uint32_t)sz+1);
            if (rb) {
                int fd = tox_open("/C:/etc/reg", 1);
                int nn = tox_read(fd, (uint8_t*)rb, (uint32_t)sz);
                tox_close(fd); if(nn<0)nn=0; rb[nn]=0;
                const char* p = rb;
                while (*p) {
                    if (p[0]=='h'&&p[1]=='o'&&p[2]=='s'&&p[3]=='t'&&
                        p[4]=='n'&&p[5]=='a'&&p[6]=='m'&&p[7]=='e'&&p[8]=='=') {
                        p+=9; char hn[64]; int hi=0;
                        while(*p&&*p!='\n'&&hi<63) hn[hi++]=*p++;
                        hn[hi]=0; if(hi>0) tox_setenv("hostname",hn); break;
                    }
                    while(*p&&*p!='\n') p++;
                    if(*p=='\n') p++;
                }
                free(rb);
            }
        }
    }

    while (1) {
        tox_setenv("USER", "toxenos");
        tox_set_admin(1);
        int pid = tox_spawn_embedded(0);
        if (pid < 0) tox_sleep(1000);
        else tox_wait(pid);
    }
}
