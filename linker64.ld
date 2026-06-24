OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)

/*
 * x86_64 higher-half kernel linker script — Milestone 1.
 *
 * Mirrors linker.ld's structure; the only structural change is the
 * higher-half virtual base, which moves from the 32-bit 0xC0000000 to
 * the canonical x86_64 convention 0xFFFFFFFF80000000 (top 2GB of the
 * 64-bit canonical address space). This is also the region
 * -mcmodel=kernel assumes kernel code/data is linked within.
 *
 * Physical memory layout (unchanged from the 32-bit boot path — GRUB's
 * placement behavior doesn't depend on target kernel architecture):
 *   0x100000 — .boot     (multiboot headers + 64-bit GDT + entry code) [R+X]
 *   0x101000 — .bootdata (PML4/PDPT/PD page tables)                    [R+W]
 *   0x200000 — .text     (kernel code, linked at KERNEL_VIRT_BASE64+0x200000) [R+X]
 *   ...      — .data     (kernel data)                                 [R+W]
 *   ...      — .bss      (kernel BSS + 64-bit boot stack)              [R+W]
 */

KERNEL_VIRT_BASE64 = 0xFFFFFFFF80000000;

SECTIONS
{
    /* ── .boot: entry code at physical 0x100000 (VMA == LMA) ────────────── */
    . = 0x100000;
    .boot : {
        KEEP(*(.multiboot2))
        KEEP(*(.multiboot))
        *(.boot)
    }

    /* ── .bootdata: page tables, must be writable at runtime ────────────── */
    . = ALIGN(0x1000);
    .bootdata (NOLOAD) : {
        *(.bootdata)
    }
    /* Force a new LOAD segment after bootdata by bumping the dot past 0x200000 */
    . = ALIGN(0x1000);

    /* ── High-half kernel: linked at KERNEL_VIRT_BASE64+0x200000 ─────────── */
    . = KERNEL_VIRT_BASE64 + 0x200000;

    .text : AT(0x200000) {
        *(.text*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE64) {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - KERNEL_VIRT_BASE64) {
        *(.data*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE64) {
        . = ALIGN(4096);
        *(.bss*)
        /* 64-bit boot stack: 64KB, stack_top64 is the high address (grows down) */
        . = ALIGN(16);
        stack_bottom64 = .;
        . += 65536;
        . = ALIGN(16);
        PROVIDE(stack_top64 = .);
    }

    . = ALIGN(4096);
    kernel64_end = .;

    /* Discard debug/exception handling sections (GCC emits more unwind
     * info by default on x86_64 freestanding builds than on i386, hence
     * -fno-asynchronous-unwind-tables in KFLAGS64 alongside this). */
    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
    }
}
