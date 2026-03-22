bits 32

section .multiboot2
align 8
mb2_start:
    dd 0xE85250D6           ; magic
    dd 0                    ; architecture: i386
    dd mb2_end - mb2_start  ; header length
    dd -(0xE85250D6 + 0 + (mb2_end - mb2_start))  ; checksum

    ; ── Framebuffer request tag ──────────────────────────────────────
    align 8
    dw 5                    ; type = framebuffer
    dw 0                    ; flags
    dd 20                   ; size
    dd 1024                 ; preferred width
    dd 768                  ; preferred height
    dd 32                   ; preferred bpp

    ; ── End tag ─────────────────────────────────────────────────────
    align 8
    dw 0
    dw 0
    dd 8
mb2_end:

section .multiboot
align 4
dd 0x1BADB002
dd 0
dd -(0x1BADB002)

section .bss
align 16
stack_bottom:
    resb 16384
global stack_top
stack_top:

section .data
align 8
gdt:
    dq 0x0000000000000000   ; null descriptor
    dq 0x00CF9A000000FFFF   ; 0x08 ring 0 code
    dq 0x00CF92000000FFFF   ; 0x10 ring 0 data
    dq 0x00CFFA000000FFFF   ; 0x18 ring 3 code (DPL=3)
    dq 0x00CFF2000000FFFF   ; 0x20 ring 3 data (DPL=3)
    dq 0x0000000000000000   ; 0x28 TSS (filled by tss_init)
    dq 0x0000000000000000   ; 0x30 TSS high
gdt_end:

gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

global tss_entry
tss_entry:
    dd gdt + 40

global gdt_base
gdt_base:
    dd gdt

section .text
global start
extern kernel_main

start:
    mov edi, eax    ; save multiboot magic
    mov esi, ebx    ; save multiboot info pointer

    lgdt [gdt_ptr]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x08:flush

flush:
    mov esp, stack_top
    push esi        ; mb_info_addr
    push edi        ; magic
    call kernel_main
    jmp $
