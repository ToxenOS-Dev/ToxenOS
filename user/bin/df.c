#include "../tox.h"

void _start() {
    char total_s[32], free_s[32];
    if (tox_sysctl("disk.total", total_s, sizeof(total_s)) < 0 ||
        tox_sysctl("disk.free",  free_s,  sizeof(free_s))  < 0) {
        set_color(0x0C); print("df: disk info unavailable\n"); set_color(0x07); tox_exit();
    }

    uint32_t total_kb = 0, free_kb = 0;
    for (int i = 0; total_s[i] >= '0' && total_s[i] <= '9'; i++)
        total_kb = total_kb * 10 + (uint32_t)(total_s[i] - '0');
    for (int i = 0; free_s[i]  >= '0' && free_s[i]  <= '9'; i++)
        free_kb  = free_kb  * 10 + (uint32_t)(free_s[i]  - '0');

    uint32_t used_kb = total_kb - free_kb;
    uint32_t pct     = total_kb ? (used_kb * 100) / total_kb : 0;

    set_color(0x0B); print("Filesystem   Size     Used     Free     Use%\n");
    set_color(0x07); print("/C: (TxFS)   ");
    print_int((int)(total_kb / 1024)); print("MB       ");
    print_int((int)(used_kb  / 1024)); print("MB       ");
    print_int((int)(free_kb  / 1024)); print("MB       ");
    print_int((int)pct); print("%\n");
    tox_exit();
}
