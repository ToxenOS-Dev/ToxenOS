#ifndef TLS_H
#define TLS_H

#include <stdint.h>

// ── TLS socket ────────────────────────────────────────────────────────────────
// High-level TLS client API built on top of our TCP stack + mbedTLS

#define TLS_MAX_SOCKETS  4

typedef struct tls_socket tls_socket_t;

// Connect to host:443 with TLS. Returns socket handle or -1 on error.
// hostname is used for SNI (Server Name Indication)
int  tls_connect(uint32_t ip, uint16_t port, const char* hostname);

// Send data over TLS connection
int  tls_send(int sock, const uint8_t* data, uint32_t len);

// Receive data. Returns bytes read, 0 on close, -1 on error.
int  tls_recv(int sock, uint8_t* buf, uint32_t maxlen, uint32_t timeout_ms);

// Close TLS connection
void tls_close(int sock);

// Syscall numbers (49-52)
#define SYS_TLS_CONNECT  49
#define SYS_TLS_SEND     50
#define SYS_TLS_RECV     51
#define SYS_TLS_CLOSE    52

#endif
