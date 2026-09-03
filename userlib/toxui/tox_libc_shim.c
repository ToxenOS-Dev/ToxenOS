// ToxenOS/userlib/toxui/tox_libc_shim.c — see tox_libc_shim.h.
#include "tox_libc_shim.h"

void* memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}

uint64_t tox_strlen(const char* s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}
