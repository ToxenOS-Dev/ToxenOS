// user/bin/ping.c — ICMP ping
// Usage: ping <hostname_or_ip>
#include "../tox.h"

static void print_ip(uint32_t ip) {
    print_int((ip>>24)&0xFF); print(".");
    print_int((ip>>16)&0xFF); print(".");
    print_int((ip>> 8)&0xFF); print(".");
    print_int((ip    )&0xFF);
}

void _start() {
    static char args[256];
    tox_get_args(args);
    char* p = args;
    while(*p == ' ') p++;

    char host[64]={0};
    int hi=0;
    while(*p && *p!=' ' && hi<63) host[hi++]=*p++;
    host[hi]=0;

    if(!host[0]){
        print("Usage: ping <host>\n");
        print("Example: ping google.com\n");
        tox_exit();
    }

    uint32_t ip = tox_resolve(host);
    if(!ip){
        set_color(0x0C); print("ping: could not resolve: "); print(host); print("\n");
        set_color(0x07); tox_exit();
    }

    print("PING "); print(host); print(" (");
    print_ip(ip); print(")\n");

    int sent=0, received=0;
    for(int seq=1; seq<=4; seq++) {
        sent++;
        int rtt = tox_ping(ip, (uint16_t)seq, 2000);
        if(rtt < 0) {
            print("Request timeout for seq "); print_int(seq); print("\n");
        } else {
            received++;
            print("Reply from "); print_ip(ip);
            print(": seq="); print_int(seq);
            print(" time=");
            if(rtt == 0) print("<10");
            else print_int(rtt);
            print("ms\n");
        }
        if(seq < 4) tox_sleep(1000);
    }

    print("\n--- "); print(host); print(" ping statistics ---\n");
    print_int(sent); print(" sent, ");
    print_int(received); print(" received, ");
    int loss = ((sent-received)*100)/sent;
    print_int(loss); print("% loss\n");

    tox_exit();
}
