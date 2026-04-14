#ifndef MEMMAP_H
#define MEMMAP_H

// ToxenOS virtual address space layout
//
//  0x00000000 ── null / unmapped
//  0x10000000 ── USER_ELF_BASE   (user ELF binaries)
//  0x20000000 ── USER_HEAP_BASE  (sbrk heap, grows up)
//  0xBFFFC000 ── USER_STACK_BOTTOM
//  0xC0000000 ── KERNEL_VIRT_BASE / USER_STACK_TOP
//  0xC0100000 ── kernel image (virtual)
//               kernel heap follows kernel_end
//
// NOTE: USER_ELF_BASE is used in the Makefile as -Ttext=$(USER_ELF_BASE).
//       Keep them in sync.

#include "paging.h"   // PAGE_SIZE

// ── Kernel addressing ──────────────────────────────────────────────────────────
#define KERNEL_VIRT_BASE  0xC0000000u  // kernel linked at (and runs at) this VA
#define KERNEL_PHYS_BASE  0x00100000u  // GRUB loads kernel here (1MB physical)

// Convert kernel virtual ↔ physical (only valid within kernel image / heap)
#define KVIRT_TO_PHYS(v)  ((uint32_t)(v) - KERNEL_VIRT_BASE)
#define KPHYS_TO_VIRT(p)  ((uint32_t)(p) + KERNEL_VIRT_BASE)

// Kernel stack region — sits above the heap in kernel virtual space.
// Each slot is KERNEL_STACK_SIZE + one guard page (PAGE_SIZE).
// The guard page is left unmapped so stack overflow causes a clean kernel panic.
#define KSTACK_VIRT_BASE   0xC14FE000u   // start of kernel stack region
#define KSTACK_SLOT_SIZE   (65536u + 4096u)  // stack + guard page per process
#define USER_ELF_BASE    0x10000000u   // all user ELF binaries linked here
#define USER_HEAP_BASE   0x20000000u   // sbrk heap starts here
#define USER_STACK_TOP   0xC0000000u   // top of user stack (= KERNEL_VIRT_BASE)
#define USER_STACK_PAGES 4             // 4 pages = 16 KB per-process user stack

// Derived
#define USER_STACK_BOTTOM  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_GUARD_PAGE    (USER_STACK_BOTTOM - PAGE_SIZE)

// Maximum size of a user ELF loaded via exec/spawn (4 MB)
#define USER_ELF_MAX_SIZE  (4u * 1024u * 1024u)

#endif // MEMMAP_H
