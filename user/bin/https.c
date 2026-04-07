// user/bin/https.c — HTTPS GET using TLS
// Usage: https <ip> <hostname> [path]
// Example: https 142.250.80.46 www.google.com /
#include "../tox.h"

#define IP4(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

static void print_ip(uint32_t ip) {
    print_int((ip>>24)&0xFF); print(".");
    print_int((ip>>16)&0xFF); print(".");
    print_int((ip>> 8)&0xFF); print(".");
    print_int((ip    )&0xFF);
}

static uint32_t parse_ip(const char* s) {
    uint32_t r=0;
    for(int i=0;i<4;i++){
        uint32_t n=0;
        while(*s>='0'&&*s<='9'){n=n*10+(*s-'0');s++;}
        if(*s=='.') s++;
        r=(r<<8)|(n&0xFF);
    }
    return r;
}

static int slen(const char* s){int i=0;while(s[i])i++;return i;}
static void scopy(char* d,const char* s){int i=0;while(s[i]){d[i]=s[i];i++;}d[i]=0;}
static void scat(char* d,const char* s){scopy(d+slen(d),s);}

static uint8_t rxbuf[2048];

void _start() {
    static char args[256];
    get_args(args);
    char* p = args;
    while(*p==' ') p++;
    // Skip to first digit (start of IP)
    while(*p && (*p<'0'||*p>'9')) p++;

    char ip_str[32]={0}, hostname[128]={0}, path[128]="/";
    int i=0;
    // Parse IP
    while(*p&&*p!=' '&&i<31) ip_str[i++]=*p++;
    ip_str[i]=0; while(*p==' ')p++;
    // Parse hostname
    i=0; while(*p&&*p!=' '&&i<127) hostname[i++]=*p++;
    hostname[i]=0; while(*p==' ')p++;
    // Parse path
    if(*p) scopy(path, p);

    if(!ip_str[0]){
        print("Usage: https <ip> <hostname> [path]\n");
        print("Example: https 142.250.80.46 www.google.com /\n");
        tox_exit();
    }

    uint32_t dst_ip = parse_ip(ip_str);
    print("Connecting to "); print_ip(dst_ip);
    print(":443 ("); print(hostname[0]?hostname:ip_str); print(")...\n");

    int sock = tox_tls_connect(dst_ip, 443, hostname[0]?hostname:0);
    if(sock < 0){
        set_color(0x0C); print("TLS connection failed\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x0A); print("TLS connected!\n"); set_color(0x07);

    // Build HTTP/1.0 GET
    static char req[512];
    scopy(req, "GET "); scat(req, path);
    scat(req, " HTTP/1.0\r\nHost: ");
    scat(req, hostname[0]?hostname:ip_str);
    scat(req, "\r\nConnection: close\r\n\r\n");

    tox_tls_send(sock, (const uint8_t*)req, (uint32_t)slen(req));

    // Read response
    print("--- Response ---\n");
    int total=0, n;
    while((n = tox_tls_recv(sock, rxbuf, 2047, 10000)) > 0){
        rxbuf[n]=0;
        print((char*)rxbuf);
        total+=n;
        if(total>8192){print("\n[truncated]\n");break;}
    }
    print("\n--- Done ---\n");

    tox_tls_close(sock);
    tox_exit();
}
