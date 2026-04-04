// user/bin/ping.c — ICMP ping
// Usage: ping <hostname_or_ip>
#include "../tox.h"

#define IP4(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))
#define DNS_SERVER   IP4(8,8,8,8)
#define DNS_PORT     53
#define DNS_SRCPORT  5354

static void print_ip(uint32_t ip) {
    print_int((ip>>24)&0xFF); print(".");
    print_int((ip>>16)&0xFF); print(".");
    print_int((ip>> 8)&0xFF); print(".");
    print_int((ip    )&0xFF);
}

static int is_digit(char c) { return c>='0' && c<='9'; }

static uint32_t parse_ip(const char* s) {
    uint32_t r=0;
    for(int i=0;i<4;i++){
        uint32_t n=0;
        while(is_digit(*s)){n=n*10+(*s-'0');s++;}
        if(*s=='.') s++;
        r=(r<<8)|(n&0xFF);
    }
    return r;
}

static int is_ip(const char* s) {
    // Check if string looks like a.b.c.d
    int dots=0;
    for(const char* p=s;*p;p++) {
        if(*p=='.') dots++;
        else if(!is_digit(*p)) return 0;
    }
    return dots==3;
}

// ── DNS resolve (same approach as dns.c) ─────────────────────────────────────
static uint8_t dns_tx[256], dns_rx[512];
static uint16_t dns_id = 0x4321;

static int encode_name(const char* name, uint8_t* buf) {
    int pos=0;
    while(*name){
        int len=0; const char* p=name;
        while(*p&&*p!='.'){p++;len++;}
        buf[pos++]=(uint8_t)len;
        for(int i=0;i<len;i++) buf[pos++]=(uint8_t)name[i];
        name+=len; if(*name=='.') name++;
    }
    buf[pos++]=0; return pos;
}

static uint32_t dns_resolve(const char* name) {
    int pos=0;
    dns_tx[pos++]=(uint8_t)(dns_id>>8); dns_tx[pos++]=(uint8_t)(dns_id&0xFF);
    dns_tx[pos++]=0x01; dns_tx[pos++]=0x00;
    dns_tx[pos++]=0x00; dns_tx[pos++]=0x01;
    dns_tx[pos++]=0x00; dns_tx[pos++]=0x00;
    dns_tx[pos++]=0x00; dns_tx[pos++]=0x00;
    dns_tx[pos++]=0x00; dns_tx[pos++]=0x00;
    pos+=encode_name(name, dns_tx+pos);
    dns_tx[pos++]=0x00; dns_tx[pos++]=0x01;
    dns_tx[pos++]=0x00; dns_tx[pos++]=0x01;

    for(int attempt=0;attempt<3;attempt++){
        int r=tox_net_udp_send(DNS_SERVER,DNS_SRCPORT,DNS_PORT,dns_tx,(uint16_t)pos);
        if(r<0){tox_net_poll();tox_sleep(200);continue;}
        int rlen=tox_net_udp_recv(DNS_SRCPORT,dns_rx,512,3000);
        if(rlen<12) continue;
        uint16_t ancount=(uint16_t)(dns_rx[6]<<8|dns_rx[7]);
        int p2=12;
        while(p2<rlen&&dns_rx[p2]){
            if((dns_rx[p2]&0xC0)==0xC0){p2+=2;goto skip;}
            p2+=dns_rx[p2]+1;
        } p2++; p2+=4;
        skip:;
        for(int i=0;i<ancount&&p2<rlen;i++){
            if((dns_rx[p2]&0xC0)==0xC0) p2+=2;
            else{while(p2<rlen&&dns_rx[p2])p2+=dns_rx[p2]+1;p2++;}
            if(p2+10>rlen) break;
            uint16_t type=(uint16_t)(dns_rx[p2]<<8|dns_rx[p2+1]);
            uint16_t rdlen=(uint16_t)(dns_rx[p2+8]<<8|dns_rx[p2+9]);
            p2+=10;
            if(type==1&&rdlen==4)
                return (uint32_t)(dns_rx[p2]<<24|dns_rx[p2+1]<<16|dns_rx[p2+2]<<8|dns_rx[p2+3]);
            p2+=rdlen;
        }
    }
    return 0;
}

void _start() {
    static char args[256];
    get_args(args);
    char* p = args;
    while(*p == ' ') p++;

    // Shell prepends "cwd/arg" — find the actual argument after last space
    // Args format from shell: "/C:/current/dir arg1 arg2"
    // Find last space to get to the actual argument
    char* last_space = 0;
    for(char* q = p; *q; q++) if(*q == ' ') last_space = q;
    if(last_space) p = last_space + 1;
    while(*p == ' ') p++;

    if(!*p){
        print("Usage: ping <host>\n");
        print("Example: ping google.com\n");
        tox_exit();
    }

    // Trim trailing spaces
    char host[64]={0};
    int hi=0;
    while(*p&&*p!=' '&&hi<63) host[hi++]=*p++;
    host[hi]=0;

    uint32_t ip=0;
    if(is_ip(host)) {
        ip=parse_ip(host);
    } else {
        print("Resolving "); print(host); print("...\n");
        ip=dns_resolve(host);
        if(!ip){
            set_color(0x0C); print("Could not resolve host\n"); set_color(0x07);
            tox_exit();
        }
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
