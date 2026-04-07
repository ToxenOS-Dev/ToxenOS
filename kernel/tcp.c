// ToxenOS/kernel/tcp.c
// Minimal TCP implementation — connect, send, recv, close
// Supports outgoing connections only (client side).
// No retransmission yet — relies on QEMU's reliable loopback/NAT.
#include <stdint.h>
#include "../include/tcp.h"
#include "../include/net.h"
#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/syscall.h"

static tcp_socket_t socks[TCP_MAX_SOCKETS];
static uint16_t next_ephemeral = 49152;  // ephemeral port range start

// ── Pseudo-random ISN ─────────────────────────────────────────────────────────
static uint32_t tcp_isn() {
    return timer_getticks() * 0x45D9F3B + 0x12345678;
}

// ── Checksum ──────────────────────────────────────────────────────────────────
// TCP checksum uses a pseudo-header: src_ip, dst_ip, 0, proto(6), tcp_len
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                               const uint8_t* tcphdr, uint16_t tcplen)
{
    uint32_t sum = 0;

    // Pseudo header — IPs must be in network byte order (big-endian)
    // src_ip and dst_ip are stored host-order, so swap them
    uint32_t sip = HTONL(src_ip);
    uint32_t dip = HTONL(dst_ip);

    sum += (sip >> 16) & 0xFFFF;
    sum += (sip      ) & 0xFFFF;
    sum += (dip >> 16) & 0xFFFF;
    sum += (dip      ) & 0xFFFF;
    sum += HTONS(6);          // protocol TCP
    sum += HTONS(tcplen);

    // TCP header + data (already in network byte order)
    const uint16_t* p = (const uint16_t*)tcphdr;
    uint16_t len = tcplen;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ── Send a raw TCP segment ────────────────────────────────────────────────────
static uint8_t tcp_tx_buf[sizeof(ip_hdr_t) + sizeof(tcp_hdr_t) + TCP_TXBUF_SIZE];

static void tcp_send_segment(tcp_socket_t* s, uint8_t flags,
                              const uint8_t* data, uint16_t datalen)
{
    uint16_t tcplen   = (uint16_t)(sizeof(tcp_hdr_t) + datalen);
    uint16_t ip_total = (uint16_t)(sizeof(ip_hdr_t) + tcplen);

    // Build IP header
    ip_hdr_t* ip = (ip_hdr_t*)tcp_tx_buf;
    ip->ver_ihl    = 0x45;
    ip->dscp_ecn   = 0;
    ip->total_len  = HTONS(ip_total);
    ip->id         = 0;
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->proto      = IP_PROTO_TCP;
    ip->checksum   = 0;
    ip->src_ip     = HTONL(net_ip);
    ip->dst_ip     = HTONL(s->remote_ip);
    ip->checksum   = ip_checksum(ip, sizeof(ip_hdr_t));

    // Build TCP header
    tcp_hdr_t* tcp = (tcp_hdr_t*)(tcp_tx_buf + sizeof(ip_hdr_t));
    tcp->src_port  = HTONS(s->local_port);
    tcp->dst_port  = HTONS(s->remote_port);
    tcp->seq       = HTONL(s->seq);
    tcp->ack       = (flags & TCP_ACK) ? HTONL(s->ack) : 0;
    tcp->data_off  = (sizeof(tcp_hdr_t) / 4) << 4;
    tcp->flags     = flags;
    tcp->window    = HTONS(TCP_RXBUF_SIZE);
    tcp->checksum  = 0;
    tcp->urgent    = 0;

    // Copy data
    if (data && datalen) {
        uint8_t* payload = tcp_tx_buf + sizeof(ip_hdr_t) + sizeof(tcp_hdr_t);
        for (uint16_t i = 0; i < datalen; i++) payload[i] = data[i];
    }

    // Checksum
    tcp->checksum = tcp_checksum(net_ip, s->remote_ip,
                                  (uint8_t*)tcp, tcplen);

    // Send via IP layer
    uint8_t dst_mac[6];
    uint32_t hop = ((s->remote_ip & net_mask) == (net_ip & net_mask))
                   ? s->remote_ip : net_gateway;
    if (!arp_lookup(hop, dst_mac)) {
        arp_send_request(hop);
        return;
    }

    // Build ethernet frame manually
    extern int e1000_send(const uint8_t*, uint16_t);
    extern uint8_t e1000_mac[];
    static uint8_t eth_frame[1518];
    eth_frame[0]=dst_mac[0]; eth_frame[1]=dst_mac[1]; eth_frame[2]=dst_mac[2];
    eth_frame[3]=dst_mac[3]; eth_frame[4]=dst_mac[4]; eth_frame[5]=dst_mac[5];
    eth_frame[6]=e1000_mac[0]; eth_frame[7]=e1000_mac[1]; eth_frame[8]=e1000_mac[2];
    eth_frame[9]=e1000_mac[3]; eth_frame[10]=e1000_mac[4]; eth_frame[11]=e1000_mac[5];
    eth_frame[12]=0x08; eth_frame[13]=0x00;  // IPv4
    for (uint16_t i = 0; i < ip_total; i++) eth_frame[14+i] = tcp_tx_buf[i];
    e1000_send(eth_frame, (uint16_t)(14 + ip_total));
}

// ── Receive buffer helpers ────────────────────────────────────────────────────
static uint32_t rxbuf_avail(tcp_socket_t* s) {
    // rx_head and rx_tail are unbounded; subtraction works with uint32 wrap
    return s->rx_tail - s->rx_head;
}

static void rxbuf_push(tcp_socket_t* s, const uint8_t* data, uint32_t len) {
    // Drop data if buffer would overflow
    uint32_t space = TCP_RXBUF_SIZE - rxbuf_avail(s);
    if (len > space) len = space;
    for (uint32_t i = 0; i < len; i++) {
        s->rxbuf[s->rx_tail & (TCP_RXBUF_SIZE - 1)] = data[i];
        s->rx_tail++;
    }
}

static uint32_t rxbuf_pop(tcp_socket_t* s, uint8_t* buf, uint32_t maxlen) {
    uint32_t avail = rxbuf_avail(s);
    uint32_t n = avail < maxlen ? avail : maxlen;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = s->rxbuf[s->rx_head & (TCP_RXBUF_SIZE - 1)];
        s->rx_head++;
    }
    return n;
}

// ── Incoming segment handler ──────────────────────────────────────────────────
void tcp_rx(uint32_t src_ip, const uint8_t* data, uint16_t len)
{
    if (len < sizeof(tcp_hdr_t)) return;
    const tcp_hdr_t* tcp = (const tcp_hdr_t*)data;

    uint16_t src_port = NTOHS(tcp->src_port);
    uint16_t dst_port = NTOHS(tcp->dst_port);
    uint32_t seq      = NTOHL(tcp->seq);
    uint32_t ack_num  = NTOHL(tcp->ack);
    uint8_t  flags    = tcp->flags;
    uint8_t  hdrlen   = (tcp->data_off >> 4) * 4;
    const uint8_t* payload = data + hdrlen;
    uint16_t paylen  = (uint16_t)(len - hdrlen);

    // Find matching socket
    tcp_socket_t* s = 0;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (socks[i].used &&
            socks[i].remote_ip   == src_ip   &&
            socks[i].remote_port == src_port &&
            socks[i].local_port  == dst_port) {
            s = &socks[i];
            break;
        }
    }
    if (!s) return;

    if (flags & TCP_RST) {
        s->state = TCP_CLOSED;
        s->rx_closed = 1;
        return;
    }

    switch (s->state) {
    case TCP_SYN_SENT:
        if ((flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)) {
            s->ack = seq + 1;
            s->seq = ack_num;
            s->state = TCP_ESTABLISHED;
            tcp_send_segment(s, TCP_ACK, 0, 0);
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        // Acknowledge new data
        if (paylen > 0 && seq == s->ack) {
            rxbuf_push(s, payload, paylen);
            s->ack += paylen;
            tcp_send_segment(s, TCP_ACK, 0, 0);
        }
        // Update our send window
        if (flags & TCP_ACK) {
            s->snd_una = ack_num;
        }
        // Remote closing
        if ((flags & TCP_FIN) && s->state == TCP_ESTABLISHED) {
            s->ack++;
            s->state = TCP_CLOSE_WAIT;
            s->rx_closed = 1;
            tcp_send_segment(s, TCP_ACK, 0, 0);
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TCP_ACK) s->state = TCP_FIN_WAIT2;
        if (flags & TCP_FIN) {
            s->ack++;
            tcp_send_segment(s, TCP_ACK, 0, 0);
            s->state = TCP_TIME_WAIT;
            s->rx_closed = 1;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TCP_FIN) {
            s->ack++;
            tcp_send_segment(s, TCP_ACK, 0, 0);
            s->state = TCP_TIME_WAIT;
            s->rx_closed = 1;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TCP_ACK) s->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void tcp_init() {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) socks[i].used = 0;
}

int tcp_connect(uint32_t dst_ip, uint16_t dst_port)
{
    // Find free socket
    int idx = -1;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++)
        if (!socks[i].used) { idx = i; break; }
    if (idx < 0) return -1;

    tcp_socket_t* s = &socks[idx];
    s->used        = 1;
    s->state       = TCP_SYN_SENT;
    s->local_ip    = net_ip;
    s->local_port  = next_ephemeral++;
    s->remote_ip   = dst_ip;
    s->remote_port = dst_port;
    s->seq         = tcp_isn();
    s->ack         = 0;
    s->snd_una     = s->seq;
    s->rx_head     = 0;
    s->rx_tail     = 0;
    s->tx_head     = 0;
    s->tx_tail     = 0;
    s->rx_closed   = 0;

    // Send SYN
    tcp_send_segment(s, TCP_SYN, 0, 0);
    s->seq++;  // SYN consumes one sequence number

    // Wait for SYN-ACK (10 second timeout)
    uint32_t deadline = timer_getticks() + 1000;
    while (s->state == TCP_SYN_SENT) {
        extern void net_poll();
        net_poll();
        if (timer_getticks() >= deadline) {
            s->used = 0;
            return -1;
        }
        extern void scheduler();
        scheduler();
    }

    if (s->state != TCP_ESTABLISHED) { s->used = 0; return -1; }
    return idx;
}

int tcp_send(int sock, const uint8_t* data, uint32_t len)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !socks[sock].used) return -1;
    tcp_socket_t* s = &socks[sock];
    if (s->state != TCP_ESTABLISHED) return -1;

    uint32_t sent = 0;
    while (sent < len) {
        // Send up to 1460 bytes at a time (MSS for 1500 MTU)
        uint32_t chunk = len - sent;
        if (chunk > 1460) chunk = 1460;

        tcp_send_segment(s, TCP_PSH | TCP_ACK, data + sent, (uint16_t)chunk);
        s->seq += chunk;
        sent   += chunk;

        // Wait for ACK before sending more
        uint32_t deadline = timer_getticks() + 300;
        while (s->snd_una < s->seq) {
            extern void net_poll();
            net_poll();
            if (timer_getticks() >= deadline) return (int)sent;
            extern void scheduler();
            scheduler();
        }
    }
    return (int)sent;
}

int tcp_recv(int sock, uint8_t* buf, uint32_t maxlen, uint32_t timeout_ms)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !socks[sock].used) return -1;
    tcp_socket_t* s = &socks[sock];

    uint32_t deadline = timer_getticks() + (timeout_ms + 9) / 10;
    while (rxbuf_avail(s) == 0) {
        if (s->rx_closed) return 0;  // connection closed, no more data
        extern void net_poll();
        net_poll();
        if (timer_getticks() >= deadline) return 0;
        extern void scheduler();
        scheduler();
    }
    return (int)rxbuf_pop(s, buf, maxlen);
}

void tcp_close(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !socks[sock].used) return;
    tcp_socket_t* s = &socks[sock];

    if (s->state == TCP_ESTABLISHED) {
        tcp_send_segment(s, TCP_FIN | TCP_ACK, 0, 0);
        s->seq++;
        s->state = TCP_FIN_WAIT1;

        // Wait for close to complete
        uint32_t deadline = timer_getticks() + 200;
        while (s->state != TCP_TIME_WAIT && s->state != TCP_CLOSED) {
            extern void net_poll();
            net_poll();
            if (timer_getticks() >= deadline) break;
            extern void scheduler();
            scheduler();
        }
    } else if (s->state == TCP_CLOSE_WAIT) {
        tcp_send_segment(s, TCP_FIN | TCP_ACK, 0, 0);
        s->seq++;
        s->state = TCP_LAST_ACK;
    }

    s->used  = 0;
    s->state = TCP_CLOSED;
}

void tcp_poll() {
    // Future: handle retransmission timeouts
}
