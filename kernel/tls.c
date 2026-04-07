// ToxenOS/kernel/tls.c
// TLS client socket layer — wraps mbedTLS over our TCP stack

#define MBEDTLS_CONFIG_FILE "../mbedtls/toxenos_config.h"

#include <stdint.h>
#include <stddef.h>
#include "../include/tls.h"
#include "../include/tcp.h"
#include "../include/timer.h"
#include "../include/vga.h"
#include "../include/syscall.h"
#include "../include/process.h"

// mbedTLS headers
#include "../mbedtls/include/mbedtls/ssl.h"
#include "../mbedtls/include/mbedtls/entropy.h"
#include "../mbedtls/include/mbedtls/ctr_drbg.h"
#include "../mbedtls/include/mbedtls/x509_crt.h"
#include "../mbedtls/include/mbedtls/error.h"
#include "../mbedtls/include/mbedtls/platform.h"

// ── TLS socket state ──────────────────────────────────────────────────────────
typedef struct {
    int                  used;
    int                  tcp_sock;
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt     cacert;
} tls_sock_t;

static tls_sock_t tls_socks[TLS_MAX_SOCKETS];

// ── mbedTLS I/O callbacks ─────────────────────────────────────────────────────
// These bridge mbedTLS's send/recv to our TCP stack

static int tls_tcp_send(void* ctx, const unsigned char* buf, size_t len) {
    int tcp_sock = *(int*)ctx;
    int r = tcp_send(tcp_sock, (const uint8_t*)buf, (uint32_t)len);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return r;
}

static int tls_tcp_recv(void* ctx, unsigned char* buf, size_t len) {
    int tcp_sock = *(int*)ctx;
    extern void net_poll();
    // Poll aggressively to receive all pending TCP segments
    for (int i = 0; i < 100; i++) net_poll();
    int r = tcp_recv(tcp_sock, (uint8_t*)buf, (uint32_t)len, 3000);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (r == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return r;
}

static int tls_tcp_recv_timeout(void* ctx, unsigned char* buf,
                                 size_t len, uint32_t timeout) {
    int tcp_sock = *(int*)ctx;
    extern void net_poll();
    for (int i = 0; i < 100; i++) net_poll();
    if (timeout == 0) timeout = 5000;
    int r = tcp_recv(tcp_sock, (uint8_t*)buf, (uint32_t)len, timeout);
    if (r < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    return r;
}

// ── Init ──────────────────────────────────────────────────────────────────────
static int tls_initialized = 0;

static void tls_init_once() {
    if (tls_initialized) return;
    for (int i = 0; i < TLS_MAX_SOCKETS; i++) tls_socks[i].used = 0;
    tls_initialized = 1;
}

// ── Connect ───────────────────────────────────────────────────────────────────
int tls_connect(uint32_t ip, uint16_t port, const char* hostname) {
    tls_init_once();

    // Copy hostname from user memory BEFORE switching page directories
    char host_copy[256] = {0};
    if (hostname && hostname[0]) {
        int i = 0;
        while (hostname[i] && i < 255) { host_copy[i] = hostname[i]; i++; }
    }

    // Switch to kernel page directory for the entire TLS operation.
    // TLS/RSA allocates heavily from kernel heap; user's cloned page tables
    // won't see new kernel page table entries added during allocation.
    extern uint32_t kernel_directory[];
    extern void paging_switch(uint32_t*);

    // Temporarily make this process use the kernel page directory.
    // The scheduler switches CR3 based on process->page_directory, so
    // we must update that pointer too — otherwise the scheduler restores
    // the user CR3 mid-handshake and bignum code faults on unmapped pages.
    process_t* proc = process_current();
    int saved_pid = proc->pid;  // save PID in case scheduler switches us
    uint32_t* saved_dir = proc->page_directory;
    proc->page_directory = kernel_directory;
    // Write kernel_directory directly to CR3
    __asm__ volatile(
        "mov %0, %%cr3\n"
        :: "r"(kernel_directory) : "memory"
    );

    // Find free socket
    int idx = -1;
    for (int i = 0; i < TLS_MAX_SOCKETS; i++)
        if (!tls_socks[i].used) { idx = i; break; }
    if (idx < 0) {
        { extern process_t* process_get_by_pid(int); process_t* p2 = process_get_by_pid(saved_pid); if(p2) p2->page_directory = saved_dir; paging_switch(saved_dir); }
        return -1;
    }

    tls_sock_t* s = &tls_socks[idx];
    s->used = 1;

    extern void klog(const char*);

    // 1. Open TCP connection (do this BEFORE switching page directory)
    klog("TLS: connecting TCP...\n");
    s->tcp_sock = tcp_connect(ip, port);
    if (s->tcp_sock < 0) {
        klog("TLS: TCP connect failed\n");
        s->used = 0;
        { extern process_t* process_get_by_pid(int); process_t* p2 = process_get_by_pid(saved_pid); if(p2) p2->page_directory = saved_dir; paging_switch(saved_dir); }
        return -1;
    }
    klog("TLS: TCP ok, init mbedtls...\n");

    // Now switch to kernel page directory for mbedTLS crypto operations.
    // TCP connect is done; from here we only do kernel heap allocations.
    proc->page_directory = kernel_directory;
    __asm__ volatile("mov %0, %%cr3\n" :: "r"(kernel_directory) : "memory");

    // 2. Init mbedTLS structures
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);
    mbedtls_x509_crt_init(&s->cacert);

    // 3. Seed RNG
    const char* pers = "toxenos_tls";
    int ret = mbedtls_ctr_drbg_seed(&s->ctr_drbg, mbedtls_entropy_func,
                                     &s->entropy,
                                     (const unsigned char*)pers, 11);
    if (ret != 0) { klog("TLS: RNG seed failed\n"); goto fail; }
    klog("TLS: RNG ok\n");

    // 4. Configure TLS
    ret = mbedtls_ssl_config_defaults(&s->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { klog("TLS: config failed\n"); goto fail; }
    klog("TLS: config ok\n");

    // Debug: count available cipher suites
    {
        const int* cs = mbedtls_ssl_list_ciphersuites();
        int count = 0;
        while (cs[count]) count++;
        char nb[12]; 
        int v = count; int i = 10;
        nb[11] = 0; nb[10] = '\n';
        do { nb[--i] = '0' + (v % 10); v /= 10; } while (v && i > 0);
        klog("TLS: cipher count="); klog(nb + i); 
    }

    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->ctr_drbg);

    // Use default (permissive) certificate profile that allows SHA-256
    mbedtls_ssl_conf_cert_profile(&s->conf, &mbedtls_x509_crt_profile_default);

    // Force TLS 1.2 only
    mbedtls_ssl_conf_min_version(&s->conf,
        MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&s->conf,
        MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);

    // 5. Setup SSL context
    ret = mbedtls_ssl_setup(&s->ssl, &s->conf);
    if (ret != 0) goto fail;

    // 6. Set hostname (SNI)
    if (host_copy[0]) {
        ret = mbedtls_ssl_set_hostname(&s->ssl, host_copy);
        if (ret != 0) goto fail;
    }

    // 7. Wire up I/O callbacks
    mbedtls_ssl_set_bio(&s->ssl, &s->tcp_sock,
                         tls_tcp_send, tls_tcp_recv, tls_tcp_recv_timeout);

    // 8. Perform TLS handshake
    klog("TLS: starting handshake...\n");
    int handshake_attempts = 0;
    do {
        ret = mbedtls_ssl_handshake(&s->ssl);
        handshake_attempts++;
        // Log every 10 attempts
        if (handshake_attempts % 10 == 0) {
            klog("TLS: handshake attempt ");
            char nb[4]; nb[0]='0'+(handshake_attempts/10); nb[1]='\n'; nb[2]=0;
            klog(nb);
        }
        if (handshake_attempts > 500) { ret = -1; break; }
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        // Log the error code
        extern void klog(const char*);
        klog("TLS handshake failed: -0x");
        char hbuf[9]; hbuf[8]=0;
        uint32_t v = (uint32_t)(-ret);
        const char* hx = "0123456789ABCDEF";
        for(int i=7;i>=0;i--){hbuf[i]=hx[v&0xF];v>>=4;}
        klog(hbuf); klog("\n");
        goto fail;
    }

    { extern process_t* process_get_by_pid(int); process_t* p2 = process_get_by_pid(saved_pid); if(p2) p2->page_directory = saved_dir; paging_switch(saved_dir); }
    return idx;

fail:
    tcp_close(s->tcp_sock);
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_entropy_free(&s->entropy);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    mbedtls_x509_crt_free(&s->cacert);
    s->used = 0;
    { extern process_t* process_get_by_pid(int); process_t* p2 = process_get_by_pid(saved_pid); if(p2) p2->page_directory = saved_dir; paging_switch(saved_dir); }
    return -1;
}

// ── Send ──────────────────────────────────────────────────────────────────────
int tls_send(int sock, const uint8_t* data, uint32_t len) {
    if (sock < 0 || sock >= TLS_MAX_SOCKETS || !tls_socks[sock].used) return -1;
    tls_sock_t* s = &tls_socks[sock];
    int ret;
    do {
        ret = mbedtls_ssl_write(&s->ssl, (const unsigned char*)data, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    return ret;
}

// ── Recv ──────────────────────────────────────────────────────────────────────
int tls_recv(int sock, uint8_t* buf, uint32_t maxlen, uint32_t timeout_ms) {
    if (sock < 0 || sock >= TLS_MAX_SOCKETS || !tls_socks[sock].used) return -1;
    tls_sock_t* s = &tls_socks[sock];
    (void)timeout_ms;
    int ret;
    do {
        ret = mbedtls_ssl_read(&s->ssl, (unsigned char*)buf, maxlen);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ);
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (ret == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
    if (ret < 0) return 0;  // treat all errors as EOF to get whatever we received
    return ret;
}

// ── Close ─────────────────────────────────────────────────────────────────────
void tls_close(int sock) {
    if (sock < 0 || sock >= TLS_MAX_SOCKETS || !tls_socks[sock].used) return;
    tls_sock_t* s = &tls_socks[sock];
    mbedtls_ssl_close_notify(&s->ssl);
    tcp_close(s->tcp_sock);
    mbedtls_ssl_free(&s->ssl);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_entropy_free(&s->entropy);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    mbedtls_x509_crt_free(&s->cacert);
    s->used = 0;
}
