// user/bin/dns.c — DNS A record lookup
// Usage: dns <hostname>
#include "../tox.h"

#define DNS_SERVER  ((uint32_t)(8<<24|8<<16|8<<8|8))
#define DNS_PORT    53
#define DNS_SRCPORT 5353

static uint8_t  tx_buf[256];
static uint8_t  rx_buf[512];
static uint16_t dns_id = 0x1234;

static int encode_name(const char* name, uint8_t* buf) {
    int pos = 0;
    while (*name) {
        int len = 0;
        const char* p = name;
        while (*p && *p != '.') { p++; len++; }
        buf[pos++] = (uint8_t)len;
        for (int i = 0; i < len; i++) buf[pos++] = (uint8_t)name[i];
        name += len;
        if (*name == '.') name++;
    }
    buf[pos++] = 0;
    return pos;
}

static int build_query(const char* name) {
    int pos = 0;
    tx_buf[pos++] = (uint8_t)(dns_id >> 8);
    tx_buf[pos++] = (uint8_t)(dns_id & 0xFF);
    tx_buf[pos++] = 0x01; tx_buf[pos++] = 0x00;
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;
    pos += encode_name(name, tx_buf + pos);
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;
    return pos;
}

static int parse_response(const uint8_t* buf, int len) {
    if (len < 12) return 0;
    uint16_t resp_id = (uint16_t)(buf[0] << 8 | buf[1]);
    uint16_t flags   = (uint16_t)(buf[2] << 8 | buf[3]);
    uint16_t ancount = (uint16_t)(buf[6] << 8 | buf[7]);
    if (resp_id != dns_id) return 0;
    if (flags & 0x000F) {
        if ((flags & 0xF) == 3) { set_color(0x0C); print("not found\n"); }
        else                    { set_color(0x0C); print("DNS error\n"); }
        set_color(0x07);
        return 0;
    }
    int pos = 12;
    // Skip question
    while (pos < len && buf[pos]) {
        if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; goto skip_done; }
        pos += buf[pos] + 1;
    }
    pos++; pos += 4;
    skip_done:;

    int found = 0;
    for (int i = 0; i < ancount && pos < len; i++) {
        if ((buf[pos] & 0xC0) == 0xC0) pos += 2;
        else { while (pos < len && buf[pos]) pos += buf[pos]+1; pos++; }
        if (pos + 10 > len) break;
        uint16_t type  = (uint16_t)(buf[pos]  << 8 | buf[pos+1]);
        uint16_t rdlen = (uint16_t)(buf[pos+8] << 8 | buf[pos+9]);
        pos += 10;
        if (type == 1 && rdlen == 4) {
            set_color(0x0A);
            print_int(buf[pos]); print(".");
            print_int(buf[pos+1]); print(".");
            print_int(buf[pos+2]); print(".");
            print_int(buf[pos+3]);
            set_color(0x07); print("\n");
            found++;
        }
        pos += rdlen;
    }
    return found;
}

void _start() {
    static char args[256];
    tox_get_args(args);
    char* name = args;
    while(*name == ' ') name++;
    // Shell prepends cwd — actual arg is after the last space
    char* last_space = 0;
    for(char* q = name; *q; q++) if(*q == ' ') last_space = q;
    if(last_space) name = last_space + 1;
    while(*name == ' ') name++;
    if (!name[0]) {
        print("Usage: dns <hostname>\n");
        tox_exit();
    }

    print("Querying: "); print(name); print("\n");

    int qlen = build_query(name);
    int n = 0;
    for (int attempt = 0; attempt < 3 && !n; attempt++) {
        int r = tox_net_udp_send(DNS_SERVER, DNS_SRCPORT, DNS_PORT,
                                  tx_buf, (uint16_t)qlen);
        if (r < 0) { tox_net_poll(); tox_sleep(200); tox_net_poll(); continue; }
        int rlen = tox_net_udp_recv(DNS_SRCPORT, rx_buf, 512, 3000);
        if (rlen <= 0) {
            if (attempt == 2) { set_color(0x0C); print("timeout\n"); set_color(0x07); }
            continue;
        }
        n = parse_response(rx_buf, rlen);
        if (!n && attempt == 2) { set_color(0x0C); print("no records\n"); set_color(0x07); }
    }
    tox_exit();
}
