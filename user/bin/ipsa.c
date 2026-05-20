#include "../tox.h"

static void print_ip(uint32_t ip) {
    print_int((int)((ip>>24)&0xFF)); print(".");
    print_int((int)((ip>>16)&0xFF)); print(".");
    print_int((int)((ip>> 8)&0xFF)); print(".");
    print_int((int)((ip    )&0xFF));
}

static uint32_t get_u32(const char* key) {
    char buf[32];
    if (tox_sysctl(key, buf, sizeof(buf)) < 0) return 0;
    uint32_t v = 0;
    for (int i = 0; buf[i] >= '0' && buf[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(buf[i] - '0');
    return v;
}

void _start() {
    uint32_t ip   = get_u32("net.ip");
    uint32_t gw   = get_u32("net.gw");
    uint32_t mask = get_u32("net.mask");

    set_color(0x0B); print("Network Configuration\n\n"); set_color(0x07);
    set_color(0x08); print("  Interface  : "); set_color(0x07); print("eth0 (e1000)\n");

    set_color(0x08); print("  IP Address : ");
    set_color(ip ? 0x0A : 0x0C); print_ip(ip); print("\n");

    set_color(0x08); print("  Subnet     : ");
    set_color(0x07); print_ip(mask); print("\n");

    set_color(0x08); print("  Gateway    : ");
    set_color(0x07); print_ip(gw); print("\n");

    set_color(0x08); print("  Status     : ");
    set_color(ip ? 0x0A : 0x0C);
    print(ip ? "connected" : "no link"); print("\n\n");
    set_color(0x07);
    tox_exit();
}
