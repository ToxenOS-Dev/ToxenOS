#include "../tox.h"

void _start() {
    char buf[32];
    if (tox_sysctl("uptime", buf, sizeof(buf)) < 0) {
        set_color(0x0C); print("uptime: unavailable\n"); set_color(0x07);
        tox_exit();
    }
    // buf contains seconds as decimal string
    uint32_t secs = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
        secs = secs * 10 + (uint32_t)(buf[i] - '0');

    uint32_t days  = secs / 86400; secs %= 86400;
    uint32_t hours = secs / 3600;  secs %= 3600;
    uint32_t mins  = secs / 60;    secs %= 60;

    set_color(0x0B); print("up ");
    if (days)  { set_color(0x0F); print_int(days);  set_color(0x0B); print("d "); }
    set_color(0x0F); print_int(hours); set_color(0x0B); print("h ");
    set_color(0x0F); print_int(mins);  set_color(0x0B); print("m ");
    set_color(0x0F); print_int(secs);  set_color(0x0B); print("s");
    set_color(0x07); print("\n");
    tox_exit();
}
