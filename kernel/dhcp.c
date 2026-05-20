// ToxenOS/kernel/dhcp.c — DHCP client (DISCOVER → OFFER → REQUEST → ACK)
#include <stdint.h>
#include "../include/net.h"
#include "../include/e1000.h"
#include "../include/klog.h"
#include "../include/timer.h"

// ── DHCP packet layout ────────────────────────────────────────────────────────
#define DHCP_MAGIC      0x63825363UL
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_ACK        5

#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_REQ_IP      50
#define OPT_LEASE_TIME  51
#define OPT_SUBNET      1
#define OPT_ROUTER      3
#define OPT_END         255
#define OPT_PAD         0

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[308];
} dhcp_pkt_t;

static dhcp_pkt_t dhcp_tx, dhcp_rx;

// Send a raw UDP frame bypassing ARP (needed before we have an IP)
extern int e1000_send(const uint8_t* data, uint16_t len);
extern uint8_t net_mac[6];

#define HTONS(x) ((uint16_t)(((x)>>8)|((x)<<8)))
#define HTONL(x) ((uint32_t)(((x)>>24)|(((x)>>8)&0xFF00)|(((x)<<8)&0xFF0000)|((x)<<24)))

static uint16_t ip_cksum(const void* ptr, int len) {
    const uint16_t* p = (const uint16_t*)ptr;
    uint32_t s = 0;
    while (len > 1) { s += *p++; len -= 2; }
    if (len) s += *(const uint8_t*)p;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)(~s);
}

static int dhcp_send_raw(dhcp_pkt_t* pkt, uint16_t pkt_len, uint32_t src_ip, uint32_t dst_ip) {
    // Ethernet + IP + UDP + DHCP in one buffer
    static uint8_t frame[1500];
    uint8_t* p = frame;

    // Ethernet header — broadcast
    static const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    for(int i=0;i<6;i++) p[i]=bcast[i];     // dst
    for(int i=0;i<6;i++) p[6+i]=net_mac[i]; // src
    p[12]=0x08; p[13]=0x00;                 // EtherType IP
    p+=14;

    // IP header
    uint16_t ip_total = (uint16_t)(20 + 8 + pkt_len);
    p[0]=0x45; p[1]=0; p[2]=ip_total>>8; p[3]=ip_total&0xFF;
    p[4]=0; p[5]=0; p[6]=0; p[7]=0;   // id, flags
    p[8]=64; p[9]=17;                   // TTL=64, proto=UDP
    p[10]=0; p[11]=0;                   // checksum placeholder
    // src IP
    p[12]=(uint8_t)(src_ip>>24); p[13]=(uint8_t)(src_ip>>16);
    p[14]=(uint8_t)(src_ip>>8);  p[15]=(uint8_t)(src_ip);
    // dst IP
    p[16]=(uint8_t)(dst_ip>>24); p[17]=(uint8_t)(dst_ip>>16);
    p[18]=(uint8_t)(dst_ip>>8);  p[19]=(uint8_t)(dst_ip);
    uint16_t ck = ip_cksum(p, 20);
    p[10]=ck>>8; p[11]=ck&0xFF;
    p+=20;

    // UDP header
    uint16_t udp_len = (uint16_t)(8 + pkt_len);
    p[0]=0; p[1]=68;                        // src port 68
    p[2]=0; p[3]=67;                        // dst port 67
    p[4]=udp_len>>8; p[5]=udp_len&0xFF;
    p[6]=0; p[7]=0;                         // checksum 0 (optional for IPv4)
    p+=8;

    // DHCP payload
    uint8_t* pp = (uint8_t*)pkt;
    for(uint16_t i=0;i<pkt_len;i++) p[i]=pp[i];

    return e1000_send(frame, (uint16_t)(14+ip_total));
}

// Try to receive a DHCP reply on port 68; returns msg_type or -1
static int dhcp_recv(uint32_t xid, uint32_t* offered_ip, uint32_t* server_id, uint32_t timeout_ms) {
    uint32_t deadline = timer_getticks() + timeout_ms/10;
    while (timer_getticks() < deadline) {
        net_poll();
        uint32_t from_ip = 0;
        int n = net_udp_recv(68, (uint8_t*)&dhcp_rx, sizeof(dhcp_rx), &from_ip, 0);
        if (n < 236) continue;  // minimum DHCP packet before options
        if (dhcp_rx.xid != HTONL(xid)) continue;
        if (HTONL(dhcp_rx.magic) != DHCP_MAGIC) continue;

        // Parse options
        uint8_t* opt = dhcp_rx.options;
        uint8_t msg_type = 0;
        while (*opt != OPT_END && opt < dhcp_rx.options + 308) {
            if (*opt == OPT_PAD) { opt++; continue; }
            uint8_t tag = *opt++; uint8_t len = *opt++;
            if (tag == OPT_MSG_TYPE && len >= 1) msg_type = opt[0];
            if (tag == OPT_SERVER_ID && len >= 4 && server_id)
                *server_id = ((uint32_t)opt[0]<<24)|((uint32_t)opt[1]<<16)|
                             ((uint32_t)opt[2]<<8)|opt[3];
            opt += len;
        }
        if (!msg_type) continue;
        if (offered_ip) *offered_ip = HTONL(dhcp_rx.yiaddr);
        return msg_type;
    }
    return -1;
}

// Build a DHCP DISCOVER or REQUEST packet
static uint16_t dhcp_build(uint8_t msg_type, uint32_t xid,
                            uint32_t req_ip, uint32_t server_id) {
    uint8_t* p = (uint8_t*)&dhcp_tx;
    for(int i=0;i<(int)sizeof(dhcp_tx);i++) p[i]=0;

    dhcp_tx.op=1; dhcp_tx.htype=1; dhcp_tx.hlen=6;
    dhcp_tx.xid=HTONL(xid);
    dhcp_tx.flags=HTONS(0x8000); // broadcast flag
    for(int i=0;i<6;i++) dhcp_tx.chaddr[i]=net_mac[i];
    dhcp_tx.magic=HTONL(DHCP_MAGIC);

    uint8_t* opt = dhcp_tx.options;
    // DHCP message type
    *opt++=OPT_MSG_TYPE; *opt++=1; *opt++=msg_type;
    // Server identifier (for REQUEST)
    if (server_id) {
        *opt++=OPT_SERVER_ID; *opt++=4;
        *opt++=(uint8_t)(server_id>>24); *opt++=(uint8_t)(server_id>>16);
        *opt++=(uint8_t)(server_id>>8);  *opt++=(uint8_t)(server_id);
    }
    // Requested IP (for REQUEST)
    if (req_ip) {
        *opt++=OPT_REQ_IP; *opt++=4;
        *opt++=(uint8_t)(req_ip>>24); *opt++=(uint8_t)(req_ip>>16);
        *opt++=(uint8_t)(req_ip>>8);  *opt++=(uint8_t)(req_ip);
    }
    *opt++=OPT_END;
    return (uint16_t)((opt - (uint8_t*)&dhcp_tx) + 1);
}

// Run DHCP. Returns 1 if successful (net_ip/net_gateway/net_mask updated), 0 on failure.
int dhcp_run(void) {
    uint32_t xid = timer_getticks() ^ 0xDEAD1234;
    klog("DHCP: sending DISCOVER...\n");

    // DISCOVER
    uint16_t len = dhcp_build(DHCP_DISCOVER, xid, 0, 0);
    dhcp_send_raw(&dhcp_tx, len, 0x00000000, 0xFFFFFFFF);

    // Wait for OFFER
    uint32_t offered_ip = 0, server_id = 0;
    if (dhcp_recv(xid, &offered_ip, &server_id, 3000) != DHCP_OFFER) {
        klog("DHCP: no OFFER received, using static IP\n");
        return 0;
    }
    klog("DHCP: got OFFER\n");

    // REQUEST
    len = dhcp_build(DHCP_REQUEST, xid, offered_ip, server_id);
    dhcp_send_raw(&dhcp_tx, len, 0x00000000, 0xFFFFFFFF);

    // Wait for ACK
    uint32_t ack_ip = 0;
    if (dhcp_recv(xid, &ack_ip, 0, 3000) != DHCP_ACK) {
        klog("DHCP: no ACK received\n");
        return 0;
    }

    // Apply configuration
    net_ip = ack_ip ? ack_ip : offered_ip;

    // Extract subnet mask and gateway from ACK options
    uint8_t* opt = dhcp_rx.options;
    while (*opt != OPT_END && opt < dhcp_rx.options + 308) {
        if (*opt == OPT_PAD) { opt++; continue; }
        uint8_t tag = *opt++; uint8_t olen = *opt++;
        if (tag == OPT_SUBNET && olen >= 4)
            net_mask = ((uint32_t)opt[0]<<24)|((uint32_t)opt[1]<<16)|
                       ((uint32_t)opt[2]<<8)|opt[3];
        if (tag == OPT_ROUTER && olen >= 4)
            net_gateway = ((uint32_t)opt[0]<<24)|((uint32_t)opt[1]<<16)|
                          ((uint32_t)opt[2]<<8)|opt[3];
        opt += olen;
    }

    klog("DHCP: IP assigned\n");
    return 1;
}
