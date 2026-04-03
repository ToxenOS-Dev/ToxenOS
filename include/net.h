#ifndef NET_H
#define NET_H

#include <stdint.h>

// ── Ethernet ──────────────────────────────────────────────────────────────────
#define ETH_ALEN        6
#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;
} eth_hdr_t;

// ── ARP ───────────────────────────────────────────────────────────────────────
#define ARP_HW_ETHERNET  1
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2

typedef struct __attribute__((packed)) {
    uint16_t hw_type;       // 1 = Ethernet
    uint16_t proto_type;    // 0x0800 = IPv4
    uint8_t  hw_len;        // 6
    uint8_t  proto_len;     // 4
    uint16_t op;            // 1=request, 2=reply
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} arp_pkt_t;

// ── IPv4 ──────────────────────────────────────────────────────────────────────
#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17
#define IP_PROTO_TCP    6

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;       // version (4) + IHL (5 = 20 bytes)
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ip_hdr_t;

// ── UDP ───────────────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

// ── ICMP ──────────────────────────────────────────────────────────────────────
#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

// ── Network config (set by net_init) ─────────────────────────────────────────
extern uint32_t net_ip;       // our IP (set via DHCP or static)
extern uint32_t net_gateway;  // default gateway
extern uint32_t net_mask;     // subnet mask
extern uint8_t  net_mac[ETH_ALEN];

// Helper macros
#define IP4(a,b,c,d)  ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | (uint32_t)(c)<<8 | (uint32_t)(d))
#define HTONS(x)  ((uint16_t)(((x)>>8)|((x)<<8)))
#define NTOHS(x)  HTONS(x)
#define HTONL(x)  ((((x)>>24)&0xFF)|(((x)>>8)&0xFF00)|(((x)<<8)&0xFF0000)|(((x)<<24)&0xFF000000))
#define NTOHL(x)  HTONL(x)

// ── Public API ────────────────────────────────────────────────────────────────

void net_init();
void net_poll();
uint32_t net_get_rx_count();
uint32_t net_get_tx_count();
uint32_t net_get_vq_used();   // raw RX used.idx from virtqueue

// Send a UDP packet
int  net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                  const uint8_t* data, uint16_t len);

// Register a UDP receive handler for a port
typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
                               const uint8_t* data, uint16_t len);
void net_udp_listen(uint16_t port, udp_handler_t handler);

// Open a port for blocking receive from userland
int  net_udp_open(uint16_t port);

// Blocking receive — waits up to timeout_ms for a packet on port.
// Returns bytes received, 0 on timeout, -1 on error.
int  net_udp_recv(uint16_t port, uint8_t* buf, uint16_t maxlen,
                  uint32_t* src_ip_out, uint32_t timeout_ms);

// ARP: resolve IP to MAC. Returns 1 if found, 0 if still waiting.
int  arp_lookup(uint32_t ip, uint8_t* mac_out);
void arp_send_request(uint32_t target_ip);

// IP checksum
uint16_t ip_checksum(const void* data, uint32_t len);

#endif
