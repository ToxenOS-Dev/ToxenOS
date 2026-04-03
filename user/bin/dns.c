// user/bin/dns.c — DNS A record lookup
// Usage: dns <hostname>
// Sends a real DNS query to 8.8.8.8 and prints the IP address.
#include "../tox.h"

#define DNS_SERVER  ((uint32_t)(8<<24|8<<16|8<<8|8))  // 8.8.8.8
#define DNS_PORT    53
#define DNS_SRCPORT 5353

// ── DNS packet structures ─────────────────────────────────────────────────────

// We build the query manually into a flat buffer — no structs needed.
// DNS header: ID(2) FLAGS(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2) = 12 bytes

static uint8_t  tx_buf[256];
static uint8_t  rx_buf[512];

static uint16_t dns_id = 0x1234;

// Encode a hostname into DNS wire format in buf, return bytes written.
// e.g. "example.com" -> \x07example\x03com\x00
static int encode_name(const char* name, uint8_t* buf) {
    int pos = 0;
    while (*name) {
        // find next label
        int label_len = 0;
        const char* p = name;
        while (*p && *p != '.') { p++; label_len++; }
        buf[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) buf[pos++] = (uint8_t)name[i];
        name += label_len;
        if (*name == '.') name++;
    }
    buf[pos++] = 0;  // root label
    return pos;
}

// Build a DNS A query for `name`, return packet length.
static int build_query(const char* name) {
    int pos = 0;

    // Header
    tx_buf[pos++] = (uint8_t)(dns_id >> 8);
    tx_buf[pos++] = (uint8_t)(dns_id & 0xFF);
    tx_buf[pos++] = 0x01;  // flags: recursion desired
    tx_buf[pos++] = 0x00;
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;  // QDCOUNT=1
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;  // ANCOUNT=0
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;  // NSCOUNT=0
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x00;  // ARCOUNT=0

    // Question
    pos += encode_name(name, tx_buf + pos);
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;  // QTYPE=A
    tx_buf[pos++] = 0x00; tx_buf[pos++] = 0x01;  // QCLASS=IN

    return pos;
}

// Parse DNS response, print A record IPs.
// Returns number of A records found.
static int parse_response(const uint8_t* buf, int len) {
    if (len < 12) return 0;

    uint16_t resp_id   = (uint16_t)(buf[0] << 8 | buf[1]);
    uint16_t flags     = (uint16_t)(buf[2] << 8 | buf[3]);
    uint16_t ancount   = (uint16_t)(buf[6] << 8 | buf[7]);

    if (resp_id != dns_id) { print("DNS: wrong ID\n"); return 0; }
    if (flags & 0x000F) {
        uint8_t rcode = (uint8_t)(flags & 0xF);
        if (rcode == 3) { set_color(0x0C); print("DNS: NXDOMAIN (not found)\n"); set_color(0x07); }
        else            { set_color(0x0C); print("DNS: error\n"); set_color(0x07); }
        return 0;
    }

    // Skip header (12 bytes) + question section
    int pos = 12;
    // Skip QNAME
    while (pos < len && buf[pos]) {
        if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; goto skip_done; }
        pos += buf[pos] + 1;
    }
    pos++;  // null terminator
    pos += 4;  // QTYPE + QCLASS
    skip_done:;

    int found = 0;
    for (int i = 0; i < ancount && pos < len; i++) {
        // Skip name (may be pointer)
        if ((buf[pos] & 0xC0) == 0xC0) pos += 2;
        else { while (pos < len && buf[pos]) pos += buf[pos] + 1; pos++; }

        if (pos + 10 > len) break;
        uint16_t type  = (uint16_t)(buf[pos]   << 8 | buf[pos+1]);
        uint16_t rdlen = (uint16_t)(buf[pos+8]  << 8 | buf[pos+9]);
        pos += 10;

        if (type == 1 && rdlen == 4) {  // A record
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

void _start()
{
    static char args[256];
    get_args(args);

    // The shell prepends cwd to args — find the last space-separated token
    // or just use args directly if it looks like a hostname (contains a dot)
    char* name = args;
    while (*name == ' ') name++;

    // If args starts with / it's a path — extract just the last component
    // which is the actual argument the user typed
    // Shell passes: "/C:/current/dir/hostname" so find last /
    if (name[0] == '/') {
        char* last = name;
        for (char* p = name; *p; p++) if (*p == '/') last = p + 1;
        name = last;
    }

    if (!name[0]) {
        print("Usage: dns <hostname>\n");
        print("Example: dns example.com\n");
        tox_exit();
    }

    print("Querying DNS for: "); print(name); print("\n");

    // Show RX count before query
    static uint32_t stats[3];
    __asm__ volatile("int $0x80" :: "a"(43), "b"(stats));
    print("RX packets before: "); print_int((int)stats[0]);
    print("  vq_used: "); print_int((int)stats[2]); print("\n");

    int qlen = build_query(name);

    // Try up to 3 times (first may need ARP)
    int n = 0;
    for (int attempt = 0; attempt < 3 && !n; attempt++) {
        int r = tox_net_udp_send(DNS_SERVER, DNS_SRCPORT, DNS_PORT,
                                  tx_buf, (uint16_t)qlen);
        if (r < 0) {
            // ARP miss — poll and retry
            tox_net_poll();
            tox_sleep(200);
            tox_net_poll();
            continue;
        }

        // Wait for response (up to 3 seconds)
        int rlen = tox_net_udp_recv(DNS_SRCPORT, rx_buf, 512, 3000);
        if (rlen <= 0) {
            __asm__ volatile("int $0x80" :: "a"(43), "b"(stats));
            print("RX after: "); print_int((int)stats[0]);
            print("  vq_used: "); print_int((int)stats[2]); print("\n");
            if (attempt == 2) {
                set_color(0x0C); print("DNS: no response (timeout)\n"); set_color(0x07);
            }
            continue;
        }

        n = parse_response(rx_buf, rlen);
        if (!n && attempt == 2) {
            set_color(0x0C); print("DNS: no A records found\n"); set_color(0x07);
        }
    }

    tox_exit();
}
