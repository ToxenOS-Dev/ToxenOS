; kernel/ring3_test64.asm — Milestone 3B: ring3 entry via iretq.
;
; ring3_enter64(rdi=user_rip, rsi=user_rsp) builds a full 5-qword iretq
; frame and drops into ring3. Never returns to its caller -- the only
; way back to ring 0 is the deliberate #UD the hardcoded user stub
; raises, caught by the existing ISR64/IDT64 path.
bits 64
section .text

global ring3_enter64

; Must stay in sync with include/gdt64.h (NASM can't include that C
; header directly) and with the equ constants of the same name in
; kernel/boot64.asm.
USER_CODE64_SEL equ 0x18
USER_DATA64_SEL equ 0x20

ring3_enter64:
    mov r10, rdi             ; user_rip (arg1) -- saved before ax/ds/es loads below
    mov r11, rsi             ; user_rsp (arg2)

    ; Ring3 code never touches memory through ds/es (the stub is pure
    ; control flow: ud2; jmp $), but loading the user data selector
    ; here keeps the visible CPU state consistent with "really in
    ; ring3" rather than leaving stale ring0 selectors loaded.
    mov ax, USER_DATA64_SEL | 3
    mov ds, ax
    mov es, ax

    ; iretq pops RIP,CS,RFLAGS,RSP,SS in that order, so they must be
    ; pushed highest-address-first (SS) down to lowest (RIP) -- the
    ; reverse of pop order.
    push USER_DATA64_SEL | 3   ; SS, RPL=3
    push r11                   ; RSP
    push 0x202                 ; RFLAGS: reserved bit1=1, IF=1
    push USER_CODE64_SEL | 3   ; CS, RPL=3
    push r10                   ; RIP

    iretq
