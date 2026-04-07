// Forward declarations for ToxenOS platform functions used by mbedTLS
#ifndef TOXENOS_PLATFORM_FWD_H
#define TOXENOS_PLATFORM_FWD_H
#include <stddef.h>
void* toxenos_calloc(size_t n, size_t size);
void  toxenos_free(void* ptr);
int   toxenos_snprintf(char* buf, size_t n, const char* fmt, ...);
int   toxenos_printf(const char* fmt, ...);
#endif
