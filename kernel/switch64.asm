bits 64

; kernel/switch64.asm — Milestone 3: 64-bit kernel-task context switch.
;
; void context_switch64(uint64_t* old_rsp_ptr, uint64_t* new_rsp_ptr);
;
; System V AMD64 ABI: args arrive in rdi (old_rsp_ptr), rsi (new_rsp_ptr).
; This is a plain, synchronous C function call — NOT an interrupt path —
; so it only needs to preserve the ABI's callee-saved register set:
; rbx, rbp, r12, r13, r14, r15 (the AMD64 analogue of the 32-bit
; context_switch's ebx/esi/edi/ebp). rax/rcx/rdx/rsi/rdi/r8-r11 are
; caller-saved and therefore not this function's responsibility — the
; C compiler already spills/reloads them around the call site exactly
; as it would for any other function call.
;
; No segment registers, CR3, or FPU/SSE state are touched: this kernel
; builds -mno-sse -mno-mmx, and every kernel task shares one static
; page-table set this milestone (no per-task address space yet), so
; there is nothing else to switch.
;
; This function makes no further calls and uses no SSE, so it has no
; 16-byte stack-alignment obligation of its own at the `call`/`ret`
; boundary beyond what the caller already guarantees — unlike
; isr64.asm's stubs, which pad explicitly before calling into C.

global context_switch64

section .text

context_switch64:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; *old_rsp_ptr = rsp  (save OLD task's stack ptr)
    mov rsp, [rsi]      ; rsp = *new_rsp_ptr  (switch to NEW task's stack)

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret
