#ifndef UACCESS_H
#define UACCESS_H

// uaccess.h — validate user-supplied pointers before the kernel touches them.
//
// Every syscall argument that is a pointer must be checked with one of these
// before being dereferenced.  Without this, a user process can pass a kernel
// address and read/write arbitrary kernel memory.
//
// Rules:
//   - All user virtual addresses must be below USER_ADDR_MAX.
//   - The entire range [ptr, ptr+size) must be below USER_ADDR_MAX.
//   - NULL is never valid.

#include <stdint.h>
#include "memmap.h"    // USER_STACK_TOP → USER_ADDR_MAX

// Anything at or above this address is kernel space.
#define USER_ADDR_MAX  USER_STACK_TOP

// Returns 1 if the range [ptr, ptr+size) is entirely in user space.
// Returns 0 if any byte is in kernel space or ptr is NULL.
static inline int uaccess_ok(const void* ptr, uint32_t size)
{
    uint32_t addr = (uint32_t)ptr;
    if (!addr) return 0;                        // NULL
    if (addr >= USER_ADDR_MAX) return 0;        // starts in kernel space
    if (size == 0) return 1;                    // zero-length range is fine
    if (addr + size < addr) return 0;           // wrap-around
    if (addr + size > USER_ADDR_MAX) return 0;  // end in kernel space
    return 1;
}

// Validate a NUL-terminated string pointer.
// Checks that the pointer itself is in user space, then walks until NUL or
// USER_ADDR_MAX, whichever comes first.  Returns 1 if the whole string
// (including the NUL terminator) fits in user space.
static inline int uaccess_str_ok(const char* ptr)
{
    uint32_t addr = (uint32_t)ptr;
    if (!addr) return 0;
    if (addr >= USER_ADDR_MAX) return 0;
    while (addr < USER_ADDR_MAX) {
        if (*(const char*)addr == '\0') return 1;
        addr++;
    }
    return 0;  // ran off the end without finding NUL
}

// Convenience macro — return -1 from the current function if the pointer
// check fails.  Use in syscall_handler cases before casting ebx/ecx/edx.
//
//   CHECK_USER_PTR(ebx, sizeof(int));
//   CHECK_USER_STR(ecx);
#define CHECK_USER_PTR(ptr, size) \
    do { if (!uaccess_ok((const void*)(uint32_t)(ptr), (size))) return (uint32_t)-1; } while(0)

#define CHECK_USER_STR(ptr) \
    do { if (!uaccess_str_ok((const char*)(uint32_t)(ptr))) return (uint32_t)-1; } while(0)

#endif // UACCESS_H
