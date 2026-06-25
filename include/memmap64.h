#ifndef MEMMAP64_H
#define MEMMAP64_H

#include <stdint.h>

// x86_64 higher-half kernel virtual base — the canonical convention (top
// 2GB of the 64-bit canonical address space). Must stay in sync with
// linker64.ld and with -mcmodel=kernel, which assumes kernel code/data is
// linked within this same top-2GB window.
#define KERNEL_VIRT_BASE64 0xFFFFFFFF80000000ULL

// Physical address GRUB loads the kernel image at. Unchanged from the
// 32-bit boot path (include/memmap.h's KERNEL_PHYS_BASE) — GRUB's
// placement behavior doesn't depend on target kernel architecture.
#define KERNEL_PHYS_BASE   0x100000u

// Shared high-half-pointer <-> physical-address conversion, valid for any
// kernel BSS/data symbol or any physmem64-pool page (both live inside the
// boot-time flat-mapped low-64MB window — see kernel/boot64.asm). Milestone
// 8: promoted out of kernel/exec64.c's and kernel/ring3_test64.c's private
// per-file copies into one shared definition; kernel/ring3_test64.c keeps
// its own untouched copy (that file is never modified), everything new
// uses this one. Do NOT use these on pml4/pdpt_low/pdpt_high/pd themselves
// -- those are .bootdata-section globals, already VA==PA, identity-mapped.
static inline uint64_t phys_of(const void* high_half_ptr) {
    return (uint64_t)high_half_ptr - KERNEL_VIRT_BASE64;
}

static inline uint8_t* phys_to_ptr(uint64_t phys) {
    return (uint8_t*)(phys + KERNEL_VIRT_BASE64);
}

#endif // MEMMAP64_H
