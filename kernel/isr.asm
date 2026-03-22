extern exception_handler
extern page_fault_handler
extern irq_handler
extern syscall_handler

global isr_stub

section .text

isr_stub:
    cli
    hlt
    jmp isr_stub

; Exceptions with NO error code pushed by CPU
%macro ISR_NOERR 1
global isr%1
isr%1:
    pushad
    push dword 0        ; dummy error code
    push dword %1       ; interrupt number
    call exception_handler
    add esp, 8
    popad
    iret
%endmacro

; Exceptions WITH error code pushed by CPU
; (CPU already pushed error code before we get here)
%macro ISR_ERR 1
global isr%1
isr%1:
    pushad
    push dword %1       ; interrupt number  (error code already on stack below)
    call exception_handler
    add esp, 4
    popad
    add esp, 4          ; pop the CPU-pushed error code
    iret
%endmacro

ISR_NOERR 0    ; divide by zero
ISR_NOERR 1    ; debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; breakpoint
ISR_NOERR 4    ; overflow
ISR_NOERR 5    ; bound range exceeded
ISR_NOERR 6    ; invalid opcode
ISR_NOERR 7    ; device not available
ISR_ERR   8    ; double fault          (has error code)
ISR_NOERR 9    ; coprocessor overrun
ISR_ERR   10   ; invalid TSS           (has error code)
ISR_ERR   11   ; segment not present   (has error code)
ISR_ERR   12   ; stack segment fault   (has error code)
ISR_ERR   13   ; general protection    (has error code)

; ── ISR 14: Page Fault — special handling ────────────────────────────────────
; CPU pushes: error_code, then we are here.
; CR2 holds the faulting virtual address.
global isr14
isr14:
    pushad

    ; read CR2 (faulting address) into eax
    mov eax, cr2

    ; stack at this point (top to bottom):
    ;   pushad registers (32 bytes)
    ;   [esp+32] = error code (pushed by CPU)
    ;   [esp+36] = eip of faulting instruction
    ;   [esp+40] = cs
    ;   [esp+44] = eflags

    push eax                    ; arg2: faulting address (cr2)
    push dword [esp+36]         ; arg1: error code
    call page_fault_handler
    add esp, 8

    popad
    add esp, 4                  ; pop CPU error code
    iret

ISR_NOERR 15   ; reserved
ISR_NOERR 16   ; x87 floating point
ISR_ERR   17   ; alignment check       (has error code)
ISR_NOERR 18   ; machine check
ISR_NOERR 19   ; SIMD floating point
ISR_NOERR 20   ; virtualization
ISR_ERR   21   ; control protection    (has error code)
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30   ; security exception    (has error code)
ISR_NOERR 31

; ── IRQ handlers ─────────────────────────────────────────────────────────────
%macro IRQ 2
global irq%1
irq%1:
    pushad
    push dword %2
    call irq_handler
    add esp, 4
    popad
    iret
%endmacro

IRQ 0, 32   ; timer
IRQ 1, 33   ; keyboard
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; ── Syscall (int 0x80) ───────────────────────────────────────────────────────
global isr128
isr128:
    push edx
    push ecx
    push ebx
    push eax
    call syscall_handler
    add esp, 16
    iret

; ── process_iret_trampoline ──────────────────────────────────────────────────
global process_iret_trampoline
process_iret_trampoline:
    ; This is the fake return address pushed onto new process stacks.
    ; When context_switch does its first ret into a new process, it lands here,
    ; then iret pops eip/cs/eflags off the stack to jump to the entry point.
    iret
