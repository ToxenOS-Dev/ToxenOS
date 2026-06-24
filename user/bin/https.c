// user/bin/https.c — HTTPS GET using TLS
// Usage: https <hostname> [path]
// Example: https www.google.com /
#include "../tox.h"

static void print_ip(uint32_t ip) {
    print_int((ip>>24)&0xFF); print(".");
    print_int((ip>>16)&0xFF); print(".");
    print_int((ip>> 8)&0xFF); print(".");
    print_int((ip    )&0xFF);
}

static int slen(const char* s){int i=0;while(s[i])i++;return i;}
static void scopy(char* d,const char* s){int i=0;while(s[i]){d[i]=s[i];i++;}d[i]=0;}
static void scat(char* d,const char* s){scopy(d+slen(d),s);}

static uint8_t rxbuf[2048];

void _start() {
    static char args[256];
    tox_get_args(args);
    char* p = args;
    while(*p==' ') p++;

    char hostname[128]={0}, path[128]="/";
    int i=0;
    // Parse hostname
    while(*p&&*p!=' '&&i<127) hostname[i++]=*p++;
    hostname[i]=0; while(*p==' ')p++;
    // Parse path
    if(*p) scopy(path, p);

    if(!hostname[0]){
        print("Usage: https <hostname> [path]\n");
        print("Example: https www.google.com /\n");
        tox_exit();
    }

    uint32_t dst_ip = tox_resolve(hostname);
    if(!dst_ip){
        set_color(0x0C); print("https: could not resolve: "); print(hostname); print("\n");
        set_color(0x07); tox_exit();
    }

    print("Connecting to "); print(hostname); print(" (");
    print_ip(dst_ip); print("):443...\n");

    int sock = tox_tls_connect(dst_ip, 443, hostname);
    if(sock < 0){
        set_color(0x0C); print("TLS connection failed\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x0A); print("TLS connected!\n"); set_color(0x07);

    // Build HTTP/1.0 GET
    static char req[512];
    scopy(req, "GET "); scat(req, path);
    scat(req, " HTTP/1.0\r\nHost: ");
    scat(req, hostname);
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
