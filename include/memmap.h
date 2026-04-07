#ifndef MEMMAP_H
#define MEMMAP_H

// ToxenOS user address space layout
//
// All constants that define where things live in virtual memory belong here.
// Anything that depends on another constant (e.g. heap must be above ELF)
// is expressed as a derived value so the relationship is explicit.
//
//  0x00000000 ─────────────────────────────────────────
//             (null / unmapped)
//  0x10000000 ─── USER_ELF_BASE
//             ELF segments loaded here by the linker (-Ttext)
//             Typical binary: code + data in first ~4 MB
//  0x20000000 ─── USER_HEAP_BASE
//             sbrk() heap grows upward from here
//             Gap from ELF_BASE gives ~256 MB of headroom before the heap
//  ...
//  0xBFFFC000 ─── USER_STACK_BOTTOM  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
//             User stack (grows downward), 4 pages = 16 KB
//  0xBFFFD000 ─── guard page (unmapped — catches stack overflow)
//  0xC0000000 ─── USER_STACK_TOP  /  KERNEL_BASE
//             Kernel mapped above this line (high half, not user-accessible)
//
// NOTE: USER_ELF_BASE is also used in the Makefile as -Ttext=$(USER_ELF_BASE).
//       They must stay in sync.  If you change it here, update the Makefile too.

#include "paging.h"   // PAGE_SIZE

#define USER_ELF_BASE    0x10000000u   // all user ELF binaries linked here
#define USER_HEAP_BASE   0x20000000u   // sbrk heap starts here
#define USER_STACK_TOP   0xC0000000u   // top of user stack (also kernel base)
#define USER_STACK_PAGES 4             // 4 pages = 16 KB per-process user stack

// Derived — do not define these independently elsewhere
#define USER_STACK_BOTTOM  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_GUARD_PAGE    (USER_STACK_BOTTOM - PAGE_SIZE)

// Maximum size of a user ELF loaded via exec/spawn (4 MB)
#define USER_ELF_MAX_SIZE  (4u * 1024u * 1024u)

#endif // MEMMAP_H
