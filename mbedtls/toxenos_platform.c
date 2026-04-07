// ToxenOS mbedTLS platform layer
#include <stdint.h>
#include <stddef.h>
#include "../include/mm.h"
#include "../include/timer.h"

// ── Memory ────────────────────────────────────────────────────────────────────
void* toxenos_calloc(size_t n, size_t size) {
    uint32_t total = (uint32_t)(n * size);
    void* p = kmalloc(total);
    if (p) { uint8_t* b=(uint8_t*)p; for(uint32_t i=0;i<total;i++) b[i]=0; }
    return p;
}
void toxenos_free(void* ptr) { if(ptr) kfree(ptr); }

// ── String functions mbedTLS needs ────────────────────────────────────────────
void* memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d=(uint8_t*)dst; const uint8_t* s=(const uint8_t*)src;
    for(size_t i=0;i<n;i++) d[i]=s[i]; return dst;
}
void* memset(void* dst, int c, size_t n) {
    uint8_t* d=(uint8_t*)dst;
    for(size_t i=0;i<n;i++) d[i]=(uint8_t)c; return dst;
}
int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* p=(const uint8_t*)a; const uint8_t* q=(const uint8_t*)b;
    for(size_t i=0;i<n;i++) { if(p[i]<q[i]) return -1; if(p[i]>q[i]) return 1; }
    return 0;
}
void* memmove(void* dst, const void* src, size_t n) {
    uint8_t* d=(uint8_t*)dst; const uint8_t* s=(const uint8_t*)src;
    if(d<s) { for(size_t i=0;i<n;i++) d[i]=s[i]; }
    else    { for(size_t i=n;i>0;i--) d[i-1]=s[i-1]; }
    return dst;
}
size_t strlen(const char* s) { size_t i=0; while(s[i]) i++; return i; }
int strcmp(const char* a, const char* b) {
    while(*a && *a==*b){a++;b++;} return (uint8_t)*a-(uint8_t)*b;
}
char* strchr(const char* s, int c) {
    while(*s){ if(*s==(char)c) return (char*)s; s++; } return 0;
}
int strncmp(const char* a, const char* b, size_t n) {
    for(size_t i=0;i<n;i++){
        if(a[i]!=b[i]) return (uint8_t)a[i]-(uint8_t)b[i];
        if(!a[i]) return 0;
    } return 0;
}


// ── Entropy ───────────────────────────────────────────────────────────────────
int mbedtls_hardware_poll(void* data, unsigned char* output,
                           size_t len, size_t* olen) {
    (void)data;
    // Mix timer ticks with a counter for variety
    static uint32_t counter = 0;
    uint32_t seed = timer_getticks() ^ (counter++ * 0x9E3779B9);
    for(size_t i=0;i<len;i++){
        seed^=seed<<13; seed^=seed>>17; seed^=seed<<5;
        seed+=0x6C62272E;
        output[i]=(unsigned char)(seed&0xFF);
    }
    *olen=len;
    return 0;
}

// ── Time ──────────────────────────────────────────────────────────────────────
typedef long mbedtls_time_t;
mbedtls_time_t mbedtls_time(mbedtls_time_t* t) {
    mbedtls_time_t val=1704067200; if(t)*t=val; return val;
}

// ── stdlib stubs ──────────────────────────────────────────────────────────────
// These are needed by mbedTLS entropy_poll.c — we stub them out since
// we use ENTROPY_HARDWARE_ALT
typedef void FILE;
FILE* fopen(const char* p, const char* m) { (void)p;(void)m; return 0; }
size_t fread(void* b, size_t s, size_t n, FILE* f){(void)b;(void)s;(void)n;(void)f;return 0;}
int fclose(FILE* f) { (void)f; return 0; }
void setbuf(FILE* f, char* b) { (void)f;(void)b; }
long syscall(long n,...) { (void)n; return -1; }
int* __errno_location(void) { static int e=0; return &e; }
void explicit_bzero(void* s, size_t n) { memset(s,0,n); }
int inet_pton(int af, const char* src, void* dst){(void)af;(void)src;(void)dst;return 0;}

// ── Division helpers (needed by bignum) ───────────────────────────────────────
// __udivdi3 is 64-bit unsigned division — provide a simple implementation
uint64_t __udivdi3(uint64_t a, uint64_t b) {
    if(b==0) return 0;
    uint64_t q=0, r=0;
    for(int i=63;i>=0;i--){
        r=(r<<1)|((a>>i)&1);
        if(r>=b){r-=b;q|=(1ULL<<i);}
    }
    return q;
}

// ── mbedTLS platform function stubs ──────────────────────────────────────────
int toxenos_printf(const char* fmt, ...) { (void)fmt; return 0; }
int toxenos_snprintf(char* buf, size_t n, const char* fmt, ...) {
    if(n>0) buf[0]=0; (void)fmt; return 0;
}
// Keep mbedtls_ names too for platform.c compatibility
int mbedtls_printf(const char* fmt, ...) { (void)fmt; return 0; }
int mbedtls_snprintf(char* buf, size_t n, const char* fmt, ...) {
    if(n>0) buf[0]=0; (void)fmt; return 0;
}
void mbedtls_exit(int status) { (void)status; while(1); }
