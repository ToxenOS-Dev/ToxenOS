BITS 32

SECTION .multiboot
align 4

MAGIC     equ 0x1BADB002
FLAGS     equ 0
CHECKSUM  equ -(MAGIC + FLAGS)

dd MAGIC
dd FLAGS
dd CHECKSUM

SECTION .text
global _start
extern kernel_main

_start:
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
