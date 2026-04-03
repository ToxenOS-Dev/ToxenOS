// ToxenOS/kernel/net.c
// Ethernet / ARP / IPv4 / UDP / ICMP network stack
#include <stdint.h>
#include "../include/net.h"
#include "../include/virtio_net.h"
#include "../include/vga.h"
#include "../include/syscall.h"

// ── Config ────────────────────────────────────────────────────────────────────
// QEMU user-mode networking gives us 10.0.2.x by default
// Host is always 10.0.2.2, gateway 10.0.2.2, DNS 10.0.2.3

uint32_t net_ip      = IP4(10,0,2,15);   // our IP
uint32_t net_gateway = IP4(10,0,2,2);
uint32_t net_mask    = IP4(255,255,255,0);
uint8_t  net_mac[ETH_ALEN];

// ── ARP cache ─────────────────────────────────────────────────────────────────
#define ARP_CACHE_SIZE  16

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    int      valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

static arp_entry_t* arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip)
            return &arp_cache[i];
    return 0;
}

static void arp_store(uint32_t ip, const uint8_t* mac) {
    // Overwrite existing or use first free slot
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid || arp_cache[i].ip == ip) {
            arp_cache[i].ip    = ip;
            arp_cache[i].valid = 1;
            for (int j = 0; j < ETH_ALEN; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
}

int arp_lookup(uint32_t ip, uint8_t* mac_out) {
    arp_entry_t* e = arp_find(ip);
    if (!e) return 0;
    for (int i = 0; i < ETH_ALEN; i++) mac_out[i] = e->mac[i];
    return 1;
}

// ── Checksum ──────────────────────────────────────────────────────────────────
uint16_t ip_checksum(const void* data, uint32_t len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ── UDP listener table ────────────────────────────────────────────────────────
#define UDP_LISTENERS_MAX 16

typedef struct {
    uint16_t      port;
    udp_handler_t handler;
} udp_listener_t;

static udp_listener_t udp_listeners[UDP_LISTENERS_MAX];
static int            udp_listener_count = 0;

void net_udp_listen(uint16_t port, udp_handler_t handler) {
    if (udp_listener_count < UDP_LISTENERS_MAX) {
        udp_listeners[udp_listener_count].port    = port;
        udp_listeners[udp_listener_count].handler = handler;
        udp_listener_count++;
    }
}

// ── Raw Ethernet send ─────────────────────────────────────────────────────────
static uint8_t tx_frame[1518];

static int eth_send(const uint8_t* dst_mac, uint16_t ethertype,
                    const uint8_t* payload, uint16_t payload_len)
{
    if (payload_len + sizeof(eth_hdr_t) > sizeof(tx_frame)) return -1;

    eth_hdr_t* eth = (eth_hdr_t*)tx_frame;
    for (int i = 0; i < ETH_ALEN; i++) eth->dst[i] = dst_mac[i];
    for (int i = 0; i < ETH_ALEN; i++) eth->src[i] = net_mac[i];
    eth->ethertype = HTONS(ethertype);

    uint8_t* body = tx_frame + sizeof(eth_hdr_t);
    for (uint16_t i = 0; i < payload_len; i++) body[i] = payload[i];

    return virtio_net_send(tx_frame, (uint16_t)(sizeof(eth_hdr_t) + payload_len));
}

// ── ARP ───────────────────────────────────────────────────────────────────────
static uint8_t broadcast_mac[ETH_ALEN] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void arp_send_request(uint32_t target_ip) {
    uint8_t buf[sizeof(arp_pkt_t)];
    arp_pkt_t* arp = (arp_pkt_t*)buf;

    arp->hw_type    = HTONS(ARP_HW_ETHERNET);
    arp->proto_type = HTONS(ETHERTYPE_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->op         = HTONS(ARP_OP_REQUEST);

    for (int i = 0; i < ETH_ALEN; i++) arp->sender_mac[i] = net_mac[i];
    arp->sender_ip = HTONL(net_ip);

    for (int i = 0; i < ETH_ALEN; i++) arp->target_mac[i] = 0;
    arp->target_ip = HTONL(target_ip);

    eth_send(broadcast_mac, ETHERTYPE_ARP, buf, sizeof(arp_pkt_t));
}

static void arp_send_reply(const arp_pkt_t* req) {
    uint8_t buf[sizeof(arp_pkt_t)];
    arp_pkt_t* arp = (arp_pkt_t*)buf;

    arp->hw_type    = HTONS(ARP_HW_ETHERNET);
    arp->proto_type = HTONS(ETHERTYPE_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->op         = HTONS(ARP_OP_REPLY);

    for (int i = 0; i < ETH_ALEN; i++) arp->sender_mac[i] = net_mac[i];
    arp->sender_ip = HTONL(net_ip);
    for (int i = 0; i < ETH_ALEN; i++) arp->target_mac[i] = req->sender_mac[i];
    arp->target_ip = req->sender_ip;

    eth_send(req->sender_mac, ETHERTYPE_ARP, buf, sizeof(arp_pkt_t));
}

static void handle_arp(const uint8_t* data, uint16_t len) {
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t* arp = (const arp_pkt_t*)data;

    uint16_t op = NTOHS(arp->op);
    uint32_t sender_ip = NTOHL(arp->sender_ip);

    // Always cache the sender
    arp_store(sender_ip, arp->sender_mac);

    if (op == ARP_OP_REQUEST && NTOHL(arp->target_ip) == net_ip)
        arp_send_reply(arp);
}

// ── ICMP (ping reply) ─────────────────────────────────────────────────────────
static uint8_t icmp_buf[1480];

static void handle_icmp(uint32_t src_ip, const uint8_t* data, uint16_t len) {
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t* req = (const icmp_hdr_t*)data;
    if (req->type != ICMP_ECHO_REQUEST) return;

    // Build reply: IP header + ICMP
    uint16_t ip_total = (uint16_t)(sizeof(ip_hdr_t) + len);
    ip_hdr_t* ip = (ip_hdr_t*)icmp_buf;
    ip->ver_ihl    = 0x45;
    ip->dscp_ecn   = 0;
    ip->total_len  = HTONS(ip_total);
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_ICMP;
    ip->checksum   = 0;
    ip->src_ip     = HTONL(net_ip);
    ip->dst_ip     = HTONL(src_ip);
    ip->checksum   = ip_checksum(ip, sizeof(ip_hdr_t));

    icmp_hdr_t* rep = (icmp_hdr_t*)(icmp_buf + sizeof(ip_hdr_t));
    rep->type     = ICMP_ECHO_REPLY;
    rep->code     = 0;
    rep->checksum = 0;
    rep->id       = req->id;
    rep->seq      = req->seq;

    // Copy echo payload
    const uint8_t* payload = data + sizeof(icmp_hdr_t);
    uint8_t*       dst     = icmp_buf + sizeof(ip_hdr_t) + sizeof(icmp_hdr_t);
    uint16_t       plen    = (uint16_t)(len - sizeof(icmp_hdr_t));
    for (uint16_t i = 0; i < plen; i++) dst[i] = payload[i];

    rep->checksum = ip_checksum(rep, len);

    uint8_t dst_mac[ETH_ALEN];
    if (!arp_lookup(src_ip, dst_mac)) return;
    eth_send(dst_mac, ETHERTYPE_IP, icmp_buf, ip_total);
}

// ── IPv4 ──────────────────────────────────────────────────────────────────────
static void handle_ip(const uint8_t* data, uint16_t len) {
    if (len < sizeof(ip_hdr_t)) return;
    const ip_hdr_t* ip = (const ip_hdr_t*)data;

    // Only accept packets destined for us or broadcast
    uint32_t dst = NTOHL(ip->dst_ip);
    if (dst != net_ip && dst != 0xFFFFFFFF) return;

    uint8_t  ihl     = (ip->ver_ihl & 0xF) * 4;
    uint32_t src_ip  = NTOHL(ip->src_ip);
    const uint8_t* payload = data + ihl;
    uint16_t payload_len   = (uint16_t)(NTOHS(ip->total_len) - ihl);

    if (ip->proto == IP_PROTO_ICMP) {
        handle_icmp(src_ip, payload, payload_len);
    } else if (ip->proto == IP_PROTO_UDP) {
        if (payload_len < sizeof(udp_hdr_t)) return;
        const udp_hdr_t* udp = (const udp_hdr_t*)payload;
        uint16_t dst_port = NTOHS(udp->dst_port);
        uint16_t src_port = NTOHS(udp->src_port);
        const uint8_t* udata = payload + sizeof(udp_hdr_t);
        uint16_t ulen = (uint16_t)(NTOHS(udp->length) - sizeof(udp_hdr_t));

        for (int i = 0; i < udp_listener_count; i++) {
            if (udp_listeners[i].port == dst_port)
                udp_listeners[i].handler(src_ip, src_port, udata, ulen);
        }
    }
}

// ── UDP send ──────────────────────────────────────────────────────────────────
static uint8_t udp_tx_buf[sizeof(ip_hdr_t) + sizeof(udp_hdr_t) + 1472];

int net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const uint8_t* data, uint16_t len)
{
    if (!virtio_net_ready) return -1;

    uint8_t dst_mac[ETH_ALEN];

    // Use gateway MAC for off-subnet destinations
    uint32_t next_hop = ((dst_ip & net_mask) == (net_ip & net_mask))
                        ? dst_ip : net_gateway;

    if (!arp_lookup(next_hop, dst_mac)) {
        arp_send_request(next_hop);
        return -1;  // caller should retry after a poll
    }

    uint16_t udp_len = (uint16_t)(sizeof(udp_hdr_t) + len);
    uint16_t ip_len  = (uint16_t)(sizeof(ip_hdr_t)  + udp_len);

    ip_hdr_t* ip = (ip_hdr_t*)udp_tx_buf;
    ip->ver_ihl    = 0x45;
    ip->dscp_ecn   = 0;
    ip->total_len  = HTONS(ip_len);
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_UDP;
    ip->checksum   = 0;
    ip->src_ip     = HTONL(net_ip);
    ip->dst_ip     = HTONL(dst_ip);
    ip->checksum   = ip_checksum(ip, sizeof(ip_hdr_t));

    udp_hdr_t* udp = (udp_hdr_t*)(udp_tx_buf + sizeof(ip_hdr_t));
    udp->src_port = HTONS(src_port);
    udp->dst_port = HTONS(dst_port);
    udp->length   = HTONS(udp_len);
    udp->checksum = 0;  // optional for IPv4

    uint8_t* payload = udp_tx_buf + sizeof(ip_hdr_t) + sizeof(udp_hdr_t);
    for (uint16_t i = 0; i < len; i++) payload[i] = data[i];

    return eth_send(dst_mac, ETHERTYPE_IP, udp_tx_buf, ip_len);
}

// ── Poll ──────────────────────────────────────────────────────────────────────
static uint8_t rx_frame[1518];

void net_poll() {
    if (!virtio_net_ready) return;

    int len;
    while ((len = virtio_net_recv(rx_frame)) > 0) {
        if (len < (int)sizeof(eth_hdr_t)) continue;
        eth_hdr_t* eth = (eth_hdr_t*)rx_frame;
        uint16_t et    = NTOHS(eth->ethertype);
        uint8_t* body  = rx_frame + sizeof(eth_hdr_t);
        uint16_t blen  = (uint16_t)(len - sizeof(eth_hdr_t));

        if (et == ETHERTYPE_ARP) handle_arp(body, blen);
        else if (et == ETHERTYPE_IP) handle_ip(body, blen);
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────
void net_init() {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) arp_cache[i].valid = 0;
    udp_listener_count = 0;

    // Copy MAC from virtio driver
    for (int i = 0; i < ETH_ALEN; i++) net_mac[i] = virtio_net_mac[i];

    // Pre-populate ARP cache with gateway
    // QEMU user-mode networking: gateway is always at 10.0.2.2
    // and its MAC is always 52:55:0a:00:02:02
    uint8_t gw_mac[ETH_ALEN] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    arp_store(net_gateway, gw_mac);

    klog("net: IP=10.0.2.15 GW=10.0.2.2\n");
}
