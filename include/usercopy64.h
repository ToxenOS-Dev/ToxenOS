#ifndef USERCOPY64_H
#define USERCOPY64_H

#include <stdint.h>

// Milestone 9: safe user-pointer access for the syscall layer. Every one
// of these validates (via paging64_check_user_range -- never allocates)
// before touching the user pointer at all, so a syscall handed a bad
// pointer fails cleanly instead of ever risking a kernel-mode page
// fault. Once validated, a direct memcpy/byte read through the raw user
// address is safe: the syscall runs under that exact process's own CR3
// already (traps never change CR3), and this kernel never enables SMAP.
// Each fails (-1) if there is no current process at all.

int copy_from_user64(void* kdst, uint64_t uaddr, uint64_t len);
int copy_to_user64(uint64_t udst, const void* ksrc, uint64_t len);

// Copies a NUL-terminated string from user memory into kdst (a kernel
// buffer of kdst_max bytes), validating one page at a time as the scan
// crosses page boundaries -- NOT the whole kdst_max range up front, so a
// short string sitting near the end of a validly mapped page is not
// rejected just because kdst_max would overrun into an unmapped next
// page. On success, kdst is NUL-terminated and *len_out (if non-NULL)
// is set to the string length (excluding the NUL). Fails (-1) if any
// touched page isn't mapped+user-accessible, or no NUL terminator is
// found within kdst_max-1 bytes.
int copy_user_cstr64(char* kdst, uint64_t uaddr, uint64_t kdst_max, uint64_t* len_out);

#endif // USERCOPY64_H
