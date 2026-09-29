bits 64

; kernel/isr64.asm — Milestone 2 interrupt/exception stubs.
;
; No pushad/popad/iret here (none of those exist or are valid in 64-bit
; mode) — every GPR is pushed/popped individually, and exit uses iretq.
; In long mode, iretq always pops all 5 CPU-pushed qwords (RIP/CS/RFLAGS/
; RSP/SS) regardless of whether a privilege change occurs (unlike the
; legacy 32-bit iret, which only pushed/popped SS:ESP on a ring change) —
; so the stub never touches those 5 qwords directly, just leaves them for
; iretq to consume once everything pushed on top of them is popped off.
;
; Frame layout pushed by each stub, low address (top of stack) to high:
;   r15,r14,r13,r12,r11,r10,r9,r8, rbp,rdi,rsi,rdx,rcx,rbx,rax,   <- GPRs
;   vector, error_code,                                          <- stub
;   rip, cs, rflags, rsp, ss                                     <- CPU
; This exactly matches trapframe64_t in include/isr64.h. The dispatcher
; receives a pointer to the start of this frame via rdi (System V AMD64
; ABI: first integer arg in rdi), captured right after the GPR pushes and
; before the alignment filler below. rdi's *original* interrupted-context
; value is already safely saved on the stack by the time we overwrite the
; live register with the frame pointer, and the later `pop rdi` restores
; that original value before iretq — no hazard.

extern isr64_dispatch
extern irq64_dispatch

%macro PUSH_GPRS 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POP_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
%endmacro

; %1 = dispatcher to call (isr64_dispatch or irq64_dispatch).
;
; SysV AMD64 ABI requires RSP % 16 == 0 at the point a `call` instruction
; executes. The CPU forces RSP to be 16-byte aligned immediately BEFORE
; it pushes the hardware interrupt frame (SDM Vol3 6.14.2), and that
; frame is always 5 qwords (40 bytes) for vectors with no error code, or
; 6 qwords (48 bytes) when the CPU also pushes an error code. Each stub
; then pushes 16 bytes (dummy-err + vector) in the no-error case, or 8
; bytes (vector only, error code already on the stack) in the error
; case -- both total exactly 56 bytes pushed before PUSH_GPRS. PUSH_GPRS
; adds a further 120 bytes (15 GPRs). 56 + 120 = 176, and 176 % 16 == 0,
; so RSP is already correctly aligned for `call` right after PUSH_GPRS --
; no extra padding is needed or correct here. (A previous version added
; an 8-byte filler "for future SSE code", which actually broke alignment
; to RSP % 16 == 8 instead of fixing it; removed.)
%macro COMMON_TAIL 1
    PUSH_GPRS
    mov rdi, rsp
    call %1
    POP_GPRS
    add rsp, 16          ; discard vector + error_code
    iretq
%endmacro

; Milestone 24: reusable resume point for the process scheduler
; (kernel/process64.c). A process that is not currently running always
; has its kernel stack arranged as [6 callee-saved slots][return address
; = this label][a trapframe64_t] -- real (if it was preempted mid-flight,
; the CPU/COMMON_TAIL built it for real) or fake (if it has never run
; yet, kernel/process64.c constructs one by hand with the same layout).
; context_switch64 (kernel/switch64.asm), reused unchanged for process
; switches, pops the 6 callee-saved registers and then `ret`s into
; exactly this label, which is simply COMMON_TAIL's tail half pulled out
; into its own callable target: reload the user data selectors (ring3
; code cannot use a DPL=0 selector; harmless/idempotent if they were
; already the user selectors from a previous entry), restore every GPR,
; discard the vector/error_code placeholders, and iretq into ring3.
USER_DATA64_SEL_RESUME equ 0x20
global process64_resume_trapframe
process64_resume_trapframe:
    mov ax, USER_DATA64_SEL_RESUME | 3
    mov ds, ax
    mov es, ax
    POP_GPRS
    add rsp, 16          ; discard vector + error_code
    iretq

; Exceptions that do NOT push a CPU error code: push a fake zero so every
; vector presents the same uniform frame layout.
%macro ISR64_NOERR 1
global isr64_%1
isr64_%1:
    push qword 0
    push qword %1
    COMMON_TAIL isr64_dispatch
%endmacro

; Exceptions that DO push a real CPU error code — it's already on the
; stack below this point, so the stub only pushes the vector number.
%macro ISR64_ERR 1
global isr64_%1
isr64_%1:
    push qword %1
    COMMON_TAIL isr64_dispatch
%endmacro

; %1 = IRQ number (0-15), %2 = IDT vector (32-47). IRQs never have a
; CPU-pushed error code.
%macro IRQ64 2
global irq64_%1
irq64_%1:
    push qword 0
    push qword %2
    COMMON_TAIL irq64_dispatch
%endmacro

; ── Exceptions 0-31. Error-code vectors per the x86 SDM (architecture-
; defined, identical set to the 32-bit kernel/isr.asm): 8,10,11,12,13,
; 14,17,21,30. Vector 14 (page fault) is NOT special-cased here, unlike
; the 32-bit code — see kernel/interrupt64.c for why: with a full
; trapframe64_t pointer already available, CR2 can just be read inside
; the C handler instead of smuggled through bespoke asm. ──────────────
ISR64_NOERR 0
ISR64_NOERR 1
ISR64_NOERR 2
ISR64_NOERR 3
ISR64_NOERR 4
ISR64_NOERR 5
ISR64_NOERR 6
ISR64_NOERR 7
ISR64_ERR   8
ISR64_NOERR 9
ISR64_ERR   10
ISR64_ERR   11
ISR64_ERR   12
ISR64_ERR   13
ISR64_ERR   14
ISR64_NOERR 15
ISR64_NOERR 16
ISR64_ERR   17
ISR64_NOERR 18
ISR64_NOERR 19
ISR64_NOERR 20
ISR64_ERR   21
ISR64_NOERR 22
ISR64_NOERR 23
ISR64_NOERR 24
ISR64_NOERR 25
ISR64_NOERR 26
ISR64_NOERR 27
ISR64_NOERR 28
ISR64_NOERR 29
ISR64_ERR   30
ISR64_NOERR 31

; ── IRQs 0-15 -> vectors 32-47 (master PIC: 32-39, slave: 40-47) ──────
IRQ64 0,  32
IRQ64 1,  33
IRQ64 2,  34
IRQ64 3,  35
IRQ64 4,  36
IRQ64 5,  37
IRQ64 6,  38
IRQ64 7,  39
IRQ64 8,  40
IRQ64 9,  41
IRQ64 10, 42
IRQ64 11, 43
IRQ64 12, 44
IRQ64 13, 45
IRQ64 14, 46
IRQ64 15, 47

; ── Syscall gate (int 0x80). DPL=3 in the IDT entry (idt64.c) lets ring3
; invoke this; mechanically it's just another vector through the same
; COMMON_TAIL/isr64_dispatch path used by every exception above --
; isr64_dispatch special-cases vector 128 as resumable (kernel/interrupt64.c)
; exactly like #BP, dispatching to syscall64_dispatch instead of halting. ──
ISR64_NOERR 128

; ── M+11A: generic vector stubs for the dynamic vector pool ─────────────
; One stub per allocatable vector 0x30..0xEE (0x80 is the syscall gate and
; is skipped), all funnelling into irq64_dispatch with the real vector in
; the frame. irq64_vec_stub_table[v - 0x30] holds each stub's address (0 for
; 0x80) so idt64_init() can install them without 190 extern declarations.
%macro IRQV64 1
irqv64_%1:
    push qword 0
    push qword %1
    COMMON_TAIL irq64_dispatch
%endmacro

%assign v 0x30
%rep 0xEF - 0x30
%if v != 0x80
    IRQV64 v
%endif
%assign v v+1
%endrep

section .rodata
align 8
global irq64_vec_stub_table
irq64_vec_stub_table:
%assign v 0x30
%rep 0xEF - 0x30
%if v != 0x80
    dq irqv64_ %+ v
%else
    dq 0
%endif
%assign v v+1
%endrep
section .text

; ── LAPIC spurious vector 0xFF ──────────────────────────────────────────
; A spurious LAPIC interrupt sets NO bit in the In-Service Register, so an
; EOI here would wrongly retire a real in-service interrupt. The stub
; therefore touches nothing but a counter -- no GPR saves needed because
; only memory is modified (inc leaves no register clobbered; flags are
; restored by iretq).
section .bss
align 8
global lapic64_spurious_count
lapic64_spurious_count: resq 1
section .text
global isr64_spurious
isr64_spurious:
    inc qword [rel lapic64_spurious_count]
    iretq

; ── Default catch-all for any vector without an explicit stub (>=48) ──
; A single shared stub can't know which of the many unused vectors fired
; it — it reports a sentinel vector number rather than the real one.
global isr64_default
isr64_default:
    push qword 0
    push qword 0xFFFF
    COMMON_TAIL isr64_dispatch
