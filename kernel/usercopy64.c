// kernel/usercopy64.c — Milestone 9: safe user-pointer access.
// See include/usercopy64.h for the safety argument (validate via
// paging64_check_user_range first, never allocates, then a direct copy
// through the now-confirmed-valid user pointer is safe).
#include <stdint.h>
#include "../include/usercopy64.h"
#include "../include/userproc64.h"
#include "../include/paging64.h"

int copy_from_user64(void* kdst, uint64_t uaddr, uint64_t len) {
    userproc64_t* cur = userproc64_current();
    if (!cur) return -1;
    if (paging64_check_user_range(&cur->as, uaddr, len, 0) < 0) return -1;

    uint8_t* src = (uint8_t*)uaddr;
    uint8_t* dst = (uint8_t*)kdst;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

int copy_to_user64(uint64_t udst, const void* ksrc, uint64_t len) {
    userproc64_t* cur = userproc64_current();
    if (!cur) return -1;
    if (paging64_check_user_range(&cur->as, udst, len, 1) < 0) return -1;

    const uint8_t* src = (const uint8_t*)ksrc;
    uint8_t* dst = (uint8_t*)udst;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

int copy_user_cstr64(char* kdst, uint64_t uaddr, uint64_t kdst_max, uint64_t* len_out) {
    userproc64_t* cur = userproc64_current();
    if (!cur || kdst_max == 0) return -1;

    uint64_t checked_page = ~0ULL;  // sentinel -- no page validated yet
    uint64_t i = 0;
    while (i < kdst_max - 1) {
        uint64_t addr = uaddr + i;
        uint64_t page = addr & ~0xFFFULL;
        if (page != checked_page) {
            if (paging64_check_user_range(&cur->as, page, 1, 0) < 0) return -1;
            checked_page = page;
        }
        uint8_t byte = *(volatile uint8_t*)addr;
        kdst[i] = (char)byte;
        if (byte == 0) {
            if (len_out) *len_out = i;
            return 0;
        }
        i++;
    }
    return -1;  // ran out of room without finding a NUL terminator
}
