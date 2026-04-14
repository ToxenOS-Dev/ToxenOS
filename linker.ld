ENTRY(start)

/*
 * Higher-half kernel linker script
 *
 * Physical memory layout:
 *   0x100000 — .boot    (multiboot headers + entry code)     [R+X]
 *   0x101000 — .bootdata (bootstrap page tables + GDT)        [R+W]
 *   0x200000 — .text    (kernel code, linked at 0xC0200000)  [R+X]
 *   ...      — .data    (kernel data)                         [R+W]
 *   ...      — .bss     (kernel BSS + stack)                  [R+W]
 *
 * GRUB loads us at 0x100000 and jumps to the ELF entry point (start),
 * which is in .boot at a physical address.  start sets up paging and
 * jumps to the high-half virtual address before calling kernel_main.
 */

KERNEL_VIRT_BASE = 0xC0000000;

SECTIONS
{
    /* ── .boot: entry code at physical 0x100000 (VMA == LMA, no offset) ─── */
    . = 0x100000;
    .boot : {
        KEEP(*(.multiboot2))
        KEEP(*(.multiboot))
        *(.boot)
    }

    /* ── .bootdata: page tables + GDT, must be writable at runtime ─────── */
    . = ALIGN(0x1000);
    .bootdata (NOLOAD) : {
        *(.bootdata)
    }
    /* Force a new LOAD segment after bootdata by bumping the dot past 0x200000 */
    . = ALIGN(0x1000);

    /* ── High-half kernel: linked at 0xC0200000, loaded at 0x200000 ─────── */
    . = KERNEL_VIRT_BASE + 0x200000;

    .text : AT(0x200000) {
        *(.text*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VIRT_BASE) {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - KERNEL_VIRT_BASE) {
        *(.data*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VIRT_BASE) {
        . = ALIGN(4096);
        *(.bss*)
        /* Kernel stack: 64KB, stack_top is the high address (grows down) */
        . = ALIGN(16);
        stack_bottom = .;
        . += 65536;
        . = ALIGN(16);
        PROVIDE(stack_top = .);
    }

    . = ALIGN(4096);
    kernel_end = .;

    /* Discard debug/exception handling sections */
    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
    }
}
