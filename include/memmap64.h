#ifndef MEMMAP64_H
#define MEMMAP64_H

// x86_64 higher-half kernel virtual base — the canonical convention (top
// 2GB of the 64-bit canonical address space). Must stay in sync with
// linker64.ld and with -mcmodel=kernel, which assumes kernel code/data is
// linked within this same top-2GB window.
#define KERNEL_VIRT_BASE64 0xFFFFFFFF80000000ULL

// Physical address GRUB loads the kernel image at. Unchanged from the
// 32-bit boot path (include/memmap.h's KERNEL_PHYS_BASE) — GRUB's
// placement behavior doesn't depend on target kernel architecture.
#define KERNEL_PHYS_BASE   0x100000u

#endif // MEMMAP64_H
