// ToxenOS/kernel/net.c
// Ethernet / ARP / IPv4 / UDP / ICMP
#include <stdint.h>
#include "../include/net.h"
#include "../include/e1000.h"
#include "../include/tcp.h"
#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/klog.h"

uint32_t net_ip      = IP4(10,0,2,15);
uint32_t net_gateway = IP4(10,0,2,2);
uint32_t net_mask    = IP4(255,255,255,0);
uint8_t  net_mac[ETH_ALEN];

// ── ARP cache ─────────────────────────────────────────────────────────────────
#define ARP_CACHE_SIZE 16
typedef struct { uint32_t ip; uint8_t mac[ETH_ALEN]; int valid; } arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

static arp_entry_t* arp_find(uint32_t ip) {
    for (int i=0;i<ARP_CACHE_SIZE;i++) if(arp_cache[i].valid&&arp_cache[i].ip==ip) return &arp_cache[i];
    return 0;
}
static void arp_store(uint32_t ip, const uint8_t* mac) {
    for (int i=0;i<ARP_CACHE_SIZE;i++) {
        if (!arp_cache[i].valid||arp_cache[i].ip==ip) {
            arp_cache[i].ip=ip; arp_cache[i].valid=1;
            for(int j=0;j<ETH_ALEN;j++) arp_cache[i].mac[j]=mac[j];
            return;
        }
    }
}
int arp_lookup(uint32_t ip, uint8_t* out) {
    arp_entry_t* e = arp_find(ip);
    if (!e) return 0;
    for (int i = 0; i < ETH_ALEN; i++) out[i] = e->mac[i];
    return 1;
}

// ── Checksum ──────────────────────────────────────────────────────────────────
uint16_t ip_checksum(const void* data, uint32_t len) {
    const uint16_t* p=(const uint16_t*)data; uint32_t sum=0;
    while(len>1){sum+=*p++;len-=2;}
    if(len) sum+=*(const uint8_t*)p;
    while(sum>>16) sum=(sum&0xFFFF)+(sum>>16);
    return (uint16_t)~sum;
}

// ── UDP receive slots ─────────────────────────────────────────────────────────
// Simple per-port ring: one slot, one packet buffered at a time.
#define UDP_RXSLOT_MAX  8
#define UDP_RXBUF_SIZE  512

typedef struct {
    uint16_t port;
    uint8_t  buf[UDP_RXBUF_SIZE];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
    int      ready;
    int      used;
} udp_rxslot_t;

static udp_rxslot_t udp_rxslots[UDP_RXSLOT_MAX];

static udp_rxslot_t* udp_slot_for(uint16_t port) {
    for (int i=0;i<UDP_RXSLOT_MAX;i++) if(udp_rxslots[i].used&&udp_rxslots[i].port==port) return &udp_rxslots[i];
    return 0;
}

int net_udp_open(uint16_t port) {
    if (udp_slot_for(port)) return 0;
    for (int i=0;i<UDP_RXSLOT_MAX;i++) {
        if (!udp_rxslots[i].used) { udp_rxslots[i].port=port; udp_rxslots[i].used=1; udp_rxslots[i].ready=0; return 0; }
    }
    return -1;
}

// ── Raw Ethernet send ─────────────────────────────────────────────────────────
static uint8_t tx_frame[1518];
static int eth_send(const uint8_t* dst, uint16_t et, const uint8_t* pay, uint16_t plen) {
    if (plen+sizeof(eth_hdr_t)>sizeof(tx_frame)) return -1;
    eth_hdr_t* e=(eth_hdr_t*)tx_frame;
    for(int i=0;i<ETH_ALEN;i++){e->dst[i]=dst[i];e->src[i]=net_mac[i];}
    e->ethertype=HTONS(et);
    for(uint16_t i=0;i<plen;i++) tx_frame[sizeof(eth_hdr_t)+i]=pay[i];
    return e1000_send(tx_frame,(uint16_t)(sizeof(eth_hdr_t)+plen));
}

// ── ARP ───────────────────────────────────────────────────────────────────────
static uint8_t bcast[ETH_ALEN]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void arp_send_request(uint32_t target_ip) {
    uint8_t buf[sizeof(arp_pkt_t)];
    arp_pkt_t* a=(arp_pkt_t*)buf;
    a->hw_type=HTONS(1); a->proto_type=HTONS(ETHERTYPE_IP); a->hw_len=6; a->proto_len=4;
    a->op=HTONS(ARP_OP_REQUEST);
    for(int i=0;i<ETH_ALEN;i++){a->sender_mac[i]=net_mac[i];a->target_mac[i]=0;}
    a->sender_ip=HTONL(net_ip); a->target_ip=HTONL(target_ip);
    eth_send(bcast,ETHERTYPE_ARP,buf,sizeof(arp_pkt_t));
}
static void arp_send_reply(const arp_pkt_t* req) {
    uint8_t buf[sizeof(arp_pkt_t)];
    arp_pkt_t* a=(arp_pkt_t*)buf;
    a->hw_type=HTONS(1); a->proto_type=HTONS(ETHERTYPE_IP); a->hw_len=6; a->proto_len=4;
    a->op=HTONS(ARP_OP_REPLY);
    for(int i=0;i<ETH_ALEN;i++){a->sender_mac[i]=net_mac[i];a->target_mac[i]=req->sender_mac[i];}
    a->sender_ip=HTONL(net_ip); a->target_ip=req->sender_ip;
    eth_send(req->sender_mac,ETHERTYPE_ARP,buf,sizeof(arp_pkt_t));
}
static void handle_arp(const uint8_t* d, uint16_t len) {
    if(len<sizeof(arp_pkt_t)) return;
    const arp_pkt_t* a=(const arp_pkt_t*)d;
    arp_store(NTOHL(a->sender_ip),a->sender_mac);
    if(NTOHS(a->op)==ARP_OP_REQUEST && NTOHL(a->target_ip)==net_ip) arp_send_reply(a);
}

// ── ICMP ping reply ───────────────────────────────────────────────────────────
static uint8_t icmp_buf[1480];

// Ping receive slot — stores last echo reply
static uint32_t ping_reply_ip  = 0;
static uint16_t ping_reply_seq = 0;
static uint32_t ping_reply_time = 0;
static int      ping_reply_ready = 0;

static void handle_icmp(uint32_t src_ip, const uint8_t* d, uint16_t len) {
    if(len<sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t* req=(const icmp_hdr_t*)d;

    if(req->type==ICMP_ECHO_REPLY) {
        ping_reply_ip    = src_ip;
        ping_reply_seq   = NTOHS(req->seq);
        ping_reply_time  = timer_getticks();
        ping_reply_ready = 1;
        return;
    }

    if(req->type!=ICMP_ECHO_REQUEST) return;
    uint16_t ip_total=(uint16_t)(sizeof(ip_hdr_t)+len);
    ip_hdr_t* ip=(ip_hdr_t*)icmp_buf;
    ip->ver_ihl=0x45; ip->dscp_ecn=0; ip->total_len=HTONS(ip_total);
    ip->id=0; ip->flags_frag=0; ip->ttl=64; ip->proto=IP_PROTO_ICMP;
    ip->checksum=0; ip->src_ip=HTONL(net_ip); ip->dst_ip=HTONL(src_ip);
    ip->checksum=ip_checksum(ip,sizeof(ip_hdr_t));
    icmp_hdr_t* rep=(icmp_hdr_t*)(icmp_buf+sizeof(ip_hdr_t));
    rep->type=ICMP_ECHO_REPLY; rep->code=0; rep->checksum=0;
    rep->id=req->id; rep->seq=req->seq;
    const uint8_t* pay=d+sizeof(icmp_hdr_t);
    uint8_t* dst=icmp_buf+sizeof(ip_hdr_t)+sizeof(icmp_hdr_t);
    uint16_t plen=(uint16_t)(len-sizeof(icmp_hdr_t));
    for(uint16_t i=0;i<plen;i++) dst[i]=pay[i];
    rep->checksum=ip_checksum(rep,len);
    uint8_t dmac[ETH_ALEN]; if(!arp_lookup(src_ip,dmac)) return;
    eth_send(dmac,ETHERTYPE_IP,icmp_buf,ip_total);
}

// ── IPv4 ──────────────────────────────────────────────────────────────────────
static void handle_ip(const uint8_t* d, uint16_t len) {
    if(len<sizeof(ip_hdr_t)) return;
    const ip_hdr_t* ip=(const ip_hdr_t*)d;
    uint32_t dst=NTOHL(ip->dst_ip);
    if(dst!=net_ip && dst!=0xFFFFFFFF) return;
    uint8_t ihl=(ip->ver_ihl&0xF)*4;
    uint32_t src_ip=NTOHL(ip->src_ip);
    const uint8_t* pay=d+ihl;
    uint16_t plen=(uint16_t)(NTOHS(ip->total_len)-ihl);
    if(ip->proto==IP_PROTO_ICMP) {
        handle_icmp(src_ip,pay,plen);
    } else if(ip->proto==IP_PROTO_UDP && plen>=sizeof(udp_hdr_t)) {
        const udp_hdr_t* udp=(const udp_hdr_t*)pay;
        uint16_t dport=NTOHS(udp->dst_port);
        uint16_t sport=NTOHS(udp->src_port);
        const uint8_t* udata=pay+sizeof(udp_hdr_t);
        uint16_t ulen=(uint16_t)(NTOHS(udp->length)-sizeof(udp_hdr_t));
        // Deliver to rx slot
        udp_rxslot_t* slot=udp_slot_for(dport);
        if(slot) {
            uint16_t copy=ulen<UDP_RXBUF_SIZE?ulen:UDP_RXBUF_SIZE;
            for(uint16_t i=0;i<copy;i++) slot->buf[i]=udata[i];
            slot->len=copy; slot->src_ip=src_ip; slot->src_port=sport; slot->ready=1;
        }
    } else if(ip->proto==IP_PROTO_TCP) {
        tcp_rx(src_ip, pay, plen);
    }
}

// ── UDP send ──────────────────────────────────────────────────────────────────
static uint8_t udp_tx[sizeof(ip_hdr_t)+sizeof(udp_hdr_t)+1472];
int net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const uint8_t* data, uint16_t len)
{
    if(!e1000_ready) return -1;
    uint8_t dmac[ETH_ALEN];
    uint32_t hop=((dst_ip&net_mask)==(net_ip&net_mask))?dst_ip:net_gateway;
    if(!arp_lookup(hop,dmac)){arp_send_request(hop);return -1;}
    uint16_t udp_len=(uint16_t)(sizeof(udp_hdr_t)+len);
    uint16_t ip_len=(uint16_t)(sizeof(ip_hdr_t)+udp_len);
    ip_hdr_t* ip=(ip_hdr_t*)udp_tx;
    ip->ver_ihl=0x45; ip->dscp_ecn=0; ip->total_len=HTONS(ip_len);
    ip->id=0; ip->flags_frag=0; ip->ttl=64; ip->proto=IP_PROTO_UDP;
    ip->checksum=0; ip->src_ip=HTONL(net_ip); ip->dst_ip=HTONL(dst_ip);
    ip->checksum=ip_checksum(ip,sizeof(ip_hdr_t));
    udp_hdr_t* udp=(udp_hdr_t*)(udp_tx+sizeof(ip_hdr_t));
    udp->src_port=HTONS(src_port); udp->dst_port=HTONS(dst_port);
    udp->length=HTONS(udp_len); udp->checksum=0;
    uint8_t* pay=udp_tx+sizeof(ip_hdr_t)+sizeof(udp_hdr_t);
    for(uint16_t i=0;i<len;i++) pay[i]=data[i];
    return eth_send(dmac,ETHERTYPE_IP,udp_tx,ip_len);
}

// ── ICMP ping send ────────────────────────────────────────────────────────────
// Returns round-trip time in ms, or -1 on timeout
int net_ping(uint32_t dst_ip, uint16_t seq, uint32_t timeout_ms) {
    uint8_t dst_mac[ETH_ALEN];
    uint32_t hop = ((dst_ip & net_mask) == (net_ip & net_mask))
                   ? dst_ip : net_gateway;
    if(!arp_lookup(hop, dst_mac)) {
        arp_send_request(hop);
        // wait briefly for ARP
        uint32_t arp_wait = timer_getticks() + 50;
        while(timer_getticks() < arp_wait) net_poll();
        if(!arp_lookup(hop, dst_mac)) return -1;
    }

    // Build ICMP echo request
    static uint8_t ping_pkt[sizeof(ip_hdr_t) + sizeof(icmp_hdr_t) + 32];
    uint8_t payload[32];
    for(int i=0;i<32;i++) payload[i]=(uint8_t)i;

    uint16_t icmp_len = sizeof(icmp_hdr_t) + 32;
    uint16_t ip_total = (uint16_t)(sizeof(ip_hdr_t) + icmp_len);

    ip_hdr_t* ip = (ip_hdr_t*)ping_pkt;
    ip->ver_ihl=0x45; ip->dscp_ecn=0; ip->total_len=HTONS(ip_total);
    ip->id=0; ip->flags_frag=0; ip->ttl=64; ip->proto=IP_PROTO_ICMP;
    ip->checksum=0; ip->src_ip=HTONL(net_ip); ip->dst_ip=HTONL(dst_ip);
    ip->checksum=ip_checksum(ip, sizeof(ip_hdr_t));

    icmp_hdr_t* icmp = (icmp_hdr_t*)(ping_pkt + sizeof(ip_hdr_t));
    icmp->type=ICMP_ECHO_REQUEST; icmp->code=0; icmp->checksum=0;
    icmp->id=HTONS(0x1234); icmp->seq=HTONS(seq);
    uint8_t* pay = ping_pkt + sizeof(ip_hdr_t) + sizeof(icmp_hdr_t);
    for(int i=0;i<32;i++) pay[i]=payload[i];
    icmp->checksum = ip_checksum(icmp, icmp_len);

    ping_reply_ready = 0;
    uint32_t t_send = timer_getticks();
    eth_send(dst_mac, ETHERTYPE_IP, ping_pkt, ip_total);

    // Wait for reply
    uint32_t deadline = t_send + (timeout_ms + 9) / 10;
    while(!ping_reply_ready) {
        net_poll();
        if(timer_getticks() >= deadline) return -1;
        extern void scheduler();
        scheduler();
    }

    // Return RTT in ms (ticks * 10ms per tick)
    uint32_t rtt_ticks = ping_reply_time - t_send;
    return (int)(rtt_ticks * 10);
}
int net_udp_recv(uint16_t port, uint8_t* buf, uint16_t maxlen,
                 uint32_t* src_ip_out, uint32_t timeout_ms)
{
    net_udp_open(port);
    udp_rxslot_t* slot=udp_slot_for(port);
    if(!slot) return -1;
    slot->ready=0;
    uint32_t deadline=timer_getticks()+(timeout_ms+9)/10;
    while(!slot->ready) {
        net_poll();
        if(slot->ready) break;
        if(timer_getticks()>=deadline) return 0;
        extern void scheduler();
        scheduler();
    }
    uint16_t copy=slot->len<maxlen?slot->len:maxlen;
    for(uint16_t i=0;i<copy;i++) buf[i]=slot->buf[i];
    if(src_ip_out) *src_ip_out=slot->src_ip;
    slot->ready=0;
    return copy;
}

// ── Poll ──────────────────────────────────────────────────────────────────────
static uint8_t rx_frame[1518];

void net_poll() {
    if(!e1000_ready) return;
    int len;
    while((len=e1000_recv(rx_frame))>0) {
        if(len<(int)sizeof(eth_hdr_t)) continue;
        eth_hdr_t* eth=(eth_hdr_t*)rx_frame;
        uint16_t et=NTOHS(eth->ethertype);
        uint8_t* body=rx_frame+sizeof(eth_hdr_t);
        uint16_t blen=(uint16_t)(len-sizeof(eth_hdr_t));
        if(et==ETHERTYPE_ARP) handle_arp(body,blen);
        else if(et==ETHERTYPE_IP) handle_ip(body,blen);
    }
}


// ── net_udp_listen (for kernel-internal use) ─────────────────────────────────
#define UDP_LISTENERS_MAX 8
typedef struct { uint16_t port; udp_handler_t handler; } udp_listener_t;
static udp_listener_t udp_listeners[UDP_LISTENERS_MAX];
static int udp_listener_count=0;
void net_udp_listen(uint16_t port, udp_handler_t handler) {
    if(udp_listener_count<UDP_LISTENERS_MAX){
        udp_listeners[udp_listener_count].port=port;
        udp_listeners[udp_listener_count].handler=handler;
        udp_listener_count++;
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────
void net_init() {
    for(int i=0;i<ARP_CACHE_SIZE;i++) arp_cache[i].valid=0;
    for(int i=0;i<UDP_RXSLOT_MAX;i++) udp_rxslots[i].used=0;
    udp_listener_count=0;
    for(int i=0;i<ETH_ALEN;i++) net_mac[i]=e1000_mac[i];
    // Pre-populate gateway ARP (QEMU user-mode: always 52:55:0a:00:02:02)
    uint8_t gw_mac[ETH_ALEN]={0x52,0x55,0x0a,0x00,0x02,0x02};
    arp_store(net_gateway,gw_mac);
    tcp_init();
    klog("net: IP=10.0.2.15 GW=10.0.2.2\n");
}
