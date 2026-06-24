; kernel/ring3_syscall_stub64.asm — Milestone 5: hardcoded ring3 test
; stub, calling the new sys64_write/sys64_exit syscalls instead of
; Milestone 3B's deliberate ud2. Assembled as a flat position-independent
; binary (no `org`, RIP-relative addressing throughout) and embedded
; into the kernel image via objcopy -- see the Makefile's kernel64
; target and kernel/ring3_test64.c, which copies these bytes into the
; carved-out user code page.
bits 64

ring3_stub_start:
    lea rdi, [rel msg]
    mov esi, msg_len
    mov eax, 1           ; SYS64_WRITE
    int 0x80

    mov edi, 0            ; exit code
    mov eax, 2            ; SYS64_EXIT
    int 0x80

.spin:                    ; unreachable -- sys64_exit halts forever
    jmp .spin

msg: db "hello from ring3", 10
msg_len equ $ - msg
