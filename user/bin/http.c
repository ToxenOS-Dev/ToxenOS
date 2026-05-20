// user/bin/http.c — HTTP GET client with optional body-only save
// Usage: http <ip> [path]              — print response to screen
//        http <ip> [path] -o <file>    — save body to file (strips headers)
//        http <ip> [path] -b           — print body only (for piping/redirect)
#include "../tox.h"

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

static int seq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint8_t rxbuf[4096];

// Find \r\n\r\n in buf, return offset past it (body start), or -1
static int find_body(const uint8_t* buf, int len) {
    for (int i = 0; i < len - 3; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n')
            return i + 4;
    }
    return -1;
}

void _start() {
    char args[512]; tox_get_args(args);
    char ip_str[32]={0}, path[128]="/", outfile[256]={0};
    int body_only = 0;

    // Parse: ip [path] [-o file | -b]
    // IP starts with digit so no cwd prepend
    char* p = args;
    while (*p == ' ') p++;

    // Get IP
    int i = 0;
    while (*p && *p != ' ' && i < 31) ip_str[i++] = *p++;
    ip_str[i] = 0;
    while (*p == ' ') p++;

    // Parse remaining tokens
    while (*p) {
        if (seq(p, "-o") && p[2] == ' ') {
            p += 3;
            int k = 0;
            while (*p && *p != ' ' && k < 255) outfile[k++] = *p++;
            outfile[k] = 0;
            body_only = 1;
        } else if (seq(p, "-b")) {
            body_only = 1; p += 2;
        } else if (*p == '/') {
            int k = 0;
            while (*p && *p != ' ' && k < 127) path[k++] = *p++;
            path[k] = 0;
        } else {
            while (*p && *p != ' ') p++;
        }
        while (*p == ' ') p++;
    }

    if (!ip_str[0]) {
        print("Usage: http <ip> [path] [-b | -o <file>]\n");
        tox_exit();
    }

    uint32_t dst_ip = parse_ip(ip_str);
    if (!body_only) {
        print("Connecting to "); print_int((int)((dst_ip>>24)&0xFF)); print(".");
        print_int((int)((dst_ip>>16)&0xFF)); print(".");
        print_int((int)((dst_ip>>8)&0xFF)); print(".");
        print_int((int)(dst_ip&0xFF)); print(":80...\n");
    }

    int sock = tox_tcp_connect(dst_ip, 80);
    if (sock < 0) {
        set_color(0x0C); print("http: connection failed\n"); set_color(0x07);
        tox_exit();
    }
    if (!body_only) { set_color(0x0A); print("Connected!\n"); set_color(0x07); }

    // Build request
    char req[512];
    tox_strcpy(req, "GET "); tox_strcat(req, path);
    tox_strcat(req, " HTTP/1.0\r\nHost: "); tox_strcat(req, ip_str);
    tox_strcat(req, "\r\nConnection: close\r\n\r\n");
    tox_tcp_send(sock, (const uint8_t*)req, (uint32_t)tox_strlen(req));

    // Open output file if -o
    int outfd = -1;
    if (outfile[0]) {
        outfd = tox_open(outfile, 2|4);
        if (outfd < 0) {
            set_color(0x0C); print("http: cannot create: "); print(outfile); print("\n");
            set_color(0x07); tox_tcp_close(sock); tox_exit();
        }
    }

    // Read response — strip headers if body_only or -o
    int header_done = 0;
    static uint8_t header_buf[2048];
    int header_len = 0;
    int total = 0, n;

    while ((n = tox_tcp_recv(sock, rxbuf, 4095, 8000)) > 0) {
        rxbuf[n] = 0;

        if (!header_done && body_only) {
            // Buffer until we find \r\n\r\n
            int copy = n < (int)(2047-header_len) ? n : (int)(2047-header_len);
            for (int k=0; k<copy; k++) header_buf[header_len+k] = rxbuf[k];
            header_len += copy;
            int body_start = find_body(header_buf, header_len);
            if (body_start >= 0) {
                header_done = 1;
                // Write/print the body part from this buffer
                int body_bytes = header_len - body_start;
                if (body_bytes > 0) {
                    if (outfd >= 0)
                        tox_write(outfd, header_buf + body_start, (uint32_t)body_bytes);
                    else
                        print((char*)(header_buf + body_start));
                    total += body_bytes;
                }
            }
        } else if (header_done || !body_only) {
            if (outfd >= 0)
                tox_write(outfd, rxbuf, (uint32_t)n);
            else
                print((char*)rxbuf);
            total += n;
        }

        if (total > 4*1024*1024) { print("\n[truncated at 4MB]\n"); break; }
    }

    if (outfd >= 0) tox_close(outfd);
    tox_tcp_close(sock);

    if (!body_only) {
        print("\n--- "); print_int(total); print(" bytes ---\n");
    } else if (outfile[0]) {
        set_color(0x0A); print("saved: "); print(outfile);
        print(" ("); print_int(total); print(" bytes)\n"); set_color(0x07);
    }
    tox_exit();
}
