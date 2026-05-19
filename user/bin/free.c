#include "../tox.h"

void _start() {
    char total_s[32], free_s[32];
    if (tox_sysctl("mem.total", total_s, sizeof(total_s)) < 0 ||
        tox_sysctl("mem.free",  free_s,  sizeof(free_s))  < 0) {
        set_color(0x0C); print("free: memory info unavailable\n"); set_color(0x07); tox_exit();
    }

    uint32_t total_kb = 0, free_kb = 0;
    for (int i = 0; total_s[i] >= '0' && total_s[i] <= '9'; i++)
        total_kb = total_kb * 10 + (uint32_t)(total_s[i] - '0');
    for (int i = 0; free_s[i]  >= '0' && free_s[i]  <= '9'; i++)
        free_kb  = free_kb  * 10 + (uint32_t)(free_s[i]  - '0');

    uint32_t used_kb = total_kb - free_kb;

    set_color(0x0B); print("              Total    Used     Free\n");
    set_color(0x07); print("Memory (KB)   ");
    print_int((int)total_kb); print("     ");
    print_int((int)used_kb);  print("     ");
    print_int((int)free_kb);  print("\n");
    set_color(0x08);
    print("Memory (MB)   ");
    print_int((int)(total_kb/1024)); print("       ");
    print_int((int)(used_kb /1024)); print("       ");
    print_int((int)(free_kb /1024)); print("\n");
    set_color(0x07);
    tox_exit();
}
