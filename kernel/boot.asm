bits 32

section .multiboot
align 4
dd 0x1BADB002
dd 0
dd -(0x1BADB002)

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .data
align 8
gdt:
    dq 0x0000000000000000   ; null descriptor
    dq 0x00CF9A000000FFFF   ; code segment: base=0, limit=4GB, ring 0
    dq 0x00CF92000000FFFF   ; data segment: base=0, limit=4GB, ring 0
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

section .text
global start
extern kernel_main

start:
    lgdt [gdt_ptr]

    ; reload segments
    mov ax, 0x10        ; data segment selector (entry 2, offset 0x10)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:flush      ; far jump to reload CS with code segment selector

flush:
    mov esp, stack_top
    call kernel_main
    jmp $