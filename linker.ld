ENTRY(start)

SECTIONS
{
    . = 1M;

    .multiboot2 :
    {
        KEEP(*(.multiboot2))
    }

    .multiboot :
    {
        KEEP(*(.multiboot))
    }

    .text :
    {
        *(.text*)
    }

    .data :
    {
        *(.data*)
    }

    .bss :
    {
        . = ALIGN(4096);
        *(.bss*)
    }

    . = ALIGN(4096);
    kernel_end = .;
}