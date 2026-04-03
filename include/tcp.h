#ifndef TCP_H
#define TCP_H

#include <stdint.h>

// ── TCP header ────────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   // high 4 bits = header length in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

// ── TCP flags ─────────────────────────────────────────────────────────────────
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

// ── TCP connection states ─────────────────────────────────────────────────────
typedef enum {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
} tcp_state_t;

// ── TCP socket ────────────────────────────────────────────────────────────────
#define TCP_RXBUF_SIZE  4096
#define TCP_TXBUF_SIZE  4096
#define TCP_MAX_SOCKETS 8

typedef struct {
    int         used;
    tcp_state_t state;

    uint32_t    local_ip;
    uint16_t    local_port;
    uint32_t    remote_ip;
    uint16_t    remote_port;

    uint32_t    seq;        // our next sequence number to send
    uint32_t    ack;        // next seq we expect from remote
    uint32_t    snd_una;    // oldest unacknowledged byte

    // Receive buffer
    uint8_t     rxbuf[TCP_RXBUF_SIZE];
    uint32_t    rx_head;    // read position
    uint32_t    rx_tail;    // write position
    int         rx_closed;  // remote sent FIN

    // Transmit buffer
    uint8_t     txbuf[TCP_TXBUF_SIZE];
    uint32_t    tx_head;
    uint32_t    tx_tail;
} tcp_socket_t;

// ── Public API ────────────────────────────────────────────────────────────────

void tcp_init();

// Open a TCP connection to dst_ip:dst_port. Returns socket fd or -1.
int  tcp_connect(uint32_t dst_ip, uint16_t dst_port);

// Send data on a socket. Returns bytes sent or -1.
int  tcp_send(int sock, const uint8_t* data, uint32_t len);

// Receive data. Blocks until data arrives or connection closes.
// Returns bytes read, 0 on connection closed, -1 on error.
int  tcp_recv(int sock, uint8_t* buf, uint32_t maxlen, uint32_t timeout_ms);

// Close a socket.
void tcp_close(int sock);

// Called by net.c when a TCP segment arrives.
void tcp_rx(uint32_t src_ip, const uint8_t* data, uint16_t len);

// Poll — retransmit anything pending, process timeouts.
void tcp_poll();

#endif
