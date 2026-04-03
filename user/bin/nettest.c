// user/bin/nettest.c — tests UDP networking
#include "../tox.h"

// IP4 helper (same as kernel macro but for userland)
#define IP4(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

static void print_ip(uint32_t ip) {
    print_int((ip >> 24) & 0xFF); print(".");
    print_int((ip >> 16) & 0xFF); print(".");
    print_int((ip >>  8) & 0xFF); print(".");
    print_int((ip      ) & 0xFF);
}

void _start()
{
    print("Network test\n");
    print("------------\n");

    // Test 1: get our IP
    set_color(0x0A); print("Test 1: get IP address... "); set_color(0x07);
    uint32_t ip = tox_net_get_ip();
    if (ip == 0) {
        set_color(0x0C); print("FAIL (no IP)\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x0A); print("PASS (");
    set_color(0x07); print_ip(ip);
    set_color(0x0A); print(")\n"); set_color(0x07);

    // Test 2: send UDP to QEMU host (10.0.2.2 port 9999)
    // QEMU user-mode will absorb this silently — we just test the send path
    set_color(0x0A); print("Test 2: UDP send to host... "); set_color(0x07);
    const char* msg  = "ToxenOS UDP test";
    uint32_t dst_ip  = IP4(10,0,2,2);
    int r = tox_net_udp_send(dst_ip, 5000, 9999,
                             (const uint8_t*)msg, 16);
    if (r == 0) {
        set_color(0x0A); print("PASS\n"); set_color(0x07);
    } else {
        // ARP miss on first try is normal — poll and retry
        tox_net_poll();
        tox_sleep(100);
        tox_net_poll();
        r = tox_net_udp_send(dst_ip, 5000, 9999, (const uint8_t*)msg, 16);
        set_color(r == 0 ? 0x0A : 0x0C);
        print(r == 0 ? "PASS (needed ARP)\n" : "FAIL\n");
        set_color(0x07);
    }

    // Test 3: poll for packets (mainly proves we don't crash)
    set_color(0x0A); print("Test 3: poll without crash... "); set_color(0x07);
    for (int i = 0; i < 5; i++) {
        tox_net_poll();
        tox_sleep(50);
    }
    set_color(0x0A); print("PASS\n"); set_color(0x07);

    set_color(0x0E);
    print("Network tests complete.\n");
    set_color(0x07);
    print("Run 'bmsg' to see driver log.\n");

    tox_exit();
}
