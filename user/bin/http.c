// user/bin/http.c — Simple HTTP/1.0 GET client
// Usage: http <ip> <path>
// Example: http 93.184.216.34 /   (example.com)
#include "../tox.h"

#define IP4(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

static void print_ip(uint32_t ip) {
    print_int((ip>>24)&0xFF); print(".");
    print_int((ip>>16)&0xFF); print(".");
    print_int((ip>> 8)&0xFF); print(".");
    print_int((ip    )&0xFF);
}

// Parse "a.b.c.d" string into uint32_t
static uint32_t parse_ip(const char* s) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t n = 0;
        while (*s >= '0' && *s <= '9') { n = n*10 + (*s-'0'); s++; }
        if (*s == '.') s++;
        r = (r << 8) | (n & 0xFF);
    }
    return r;
}

static int str_len(const char* s) { int i=0; while(s[i]) i++; return i; }
static void str_copy(char* d, const char* s) { int i=0; while(s[i]){d[i]=s[i];i++;} d[i]=0; }

static uint8_t rxbuf[2048];

void _start() {
    static char args[256];
    get_args(args);
    char* p = args;
    while (*p == ' ') p++;

    // Shell prepends cwd to args — skip everything up to first digit
    // since an IP address starts with a digit
    while (*p && (*p < '0' || *p > '9')) p++;

    // Parse IP and path from args: "1.2.3.4 /path"
    char ip_str[32] = {0};
    char path[128]  = "/";
    int i = 0;
    while (*p && *p != ' ' && i < 31) { ip_str[i++] = *p++; }
    ip_str[i] = 0;
    while (*p == ' ') p++;
    if (*p) str_copy(path, p);

    if (!ip_str[0]) {
        print("Usage: http <ip> [path]\n");
        print("Example: http 93.184.216.34 /\n");
        tox_exit();
    }

    uint32_t dst_ip = parse_ip(ip_str);
    print("Connecting to "); print_ip(dst_ip); print(":80...\n");

    int sock = tox_tcp_connect(dst_ip, 80);
    if (sock < 0) {
        set_color(0x0C); print("Connection failed\n"); set_color(0x07);
        tox_exit();
    }
    set_color(0x0A); print("Connected!\n"); set_color(0x07);

    // Build HTTP/1.0 GET request
    static char req[256];
    str_copy(req, "GET ");
    int rlen = str_len(req);
    str_copy(req + rlen, path); rlen = str_len(req);
    str_copy(req + rlen, " HTTP/1.0\r\nHost: "); rlen = str_len(req);
    str_copy(req + rlen, ip_str); rlen = str_len(req);
    str_copy(req + rlen, "\r\nConnection: close\r\n\r\n");

    tox_tcp_send(sock, (const uint8_t*)req, (uint32_t)str_len(req));

    // Read and print response
    print("--- Response ---\n");
    int total = 0;
    int n;
    while ((n = tox_tcp_recv(sock, rxbuf, 2047, 5000)) > 0) {
        rxbuf[n] = 0;
        print((char*)rxbuf);
        total += n;
        if (total > 8192) { print("\n[truncated]\n"); break; }
    }
    print("\n--- Done ---\n");

    tox_tcp_close(sock);
    tox_exit();
}
