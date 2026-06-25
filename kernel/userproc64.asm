; kernel/userproc64.asm — Milestone 7: asymmetric enter/resume primitive.
;
; ring3_test64.asm's ring3_enter64 is fire-and-forget: iretq, never
; returns. Milestone 7 needs a version that ALSO records a continuation
; point first, so a later sys64_exit or fault handler -- running on a
; completely different stack (the syscall/RSP0 stack) -- can abandon
; that stack and resume exactly where userproc64_enter's caller left
; off, the same way context_switch64 (Milestone 3A) resumes a previously
; saved kernel task. ring3_test64.asm/.c stay untouched; this is a new,
; parallel mechanism in new files.
bits 64
section .text

global userproc64_enter
global userproc64_return_to_kernel

; Must stay in sync with include/gdt64.h and kernel/ring3_test64.asm's
; own copies (NASM can't include the C header directly).
USER_CODE64_SEL equ 0x18
USER_DATA64_SEL equ 0x20

; userproc64_enter(rdi=&kernel_resume_rsp, rsi=user_rip, rdx=user_rsp)
; Saves the launcher's callee-saved regs + rsp into *kernel_resume_rsp
; (exactly like context_switch64's save half), then builds the same
; iretq frame ring3_enter64 does and drops into ring3.
userproc64_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp           ; record resume point before clobbering rdi

    mov r10, rsi             ; user_rip
    mov r11, rdx             ; user_rsp

    mov ax, USER_DATA64_SEL | 3
    mov ds, ax
    mov es, ax

    push USER_DATA64_SEL | 3  ; SS, RPL=3
    push r11                  ; RSP
    push 0x202                ; RFLAGS: reserved bit1=1, IF=1
    push USER_CODE64_SEL | 3  ; CS, RPL=3
    push r10                  ; RIP

    iretq

; userproc64_return_to_kernel(rdi=kernel_resume_rsp)
; Called from sys64_exit or the fault path, running on the syscall/RSP0
; stack -- abandons that stack entirely and resumes userproc64_enter's
; caller via the saved rsp, popping exactly what the push side above
; pushed, then a normal ret. Never returns to its own caller.
userproc64_return_to_kernel:
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret
