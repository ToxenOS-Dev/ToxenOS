#ifndef ISR64_H
#define ISR64_H

#include <stdint.h>

// Trap frame built by kernel/isr64.asm's ISR64_COMMON tail. Field order
// mirrors the push order exactly, lowest-stack-address (= last pushed)
// first: r15..r8, rbp, rdi, rsi, rdx, rcx, rbx, rax, then vector/error_code
// pushed by the stub itself, then the 5 qwords the CPU pushes automatically
// (rip/cs/rflags/rsp/ss — iretq always pops all 5 in long mode, even with
// no privilege change, unlike the legacy 32-bit iret).
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} trapframe64_t;

// Exception stubs (vectors 0-31) and IRQ stubs (vectors 32-47), defined
// in kernel/isr64.asm. idt64_init() installs these via their addresses.
extern void isr64_0(void),  isr64_1(void),  isr64_2(void),  isr64_3(void);
extern void isr64_4(void),  isr64_5(void),  isr64_6(void),  isr64_7(void);
extern void isr64_8(void),  isr64_9(void),  isr64_10(void), isr64_11(void);
extern void isr64_12(void), isr64_13(void), isr64_14(void), isr64_15(void);
extern void isr64_16(void), isr64_17(void), isr64_18(void), isr64_19(void);
extern void isr64_20(void), isr64_21(void), isr64_22(void), isr64_23(void);
extern void isr64_24(void), isr64_25(void), isr64_26(void), isr64_27(void);
extern void isr64_28(void), isr64_29(void), isr64_30(void), isr64_31(void);

extern void irq64_0(void),  irq64_1(void),  irq64_2(void),  irq64_3(void);
extern void irq64_4(void),  irq64_5(void),  irq64_6(void),  irq64_7(void);
extern void irq64_8(void),  irq64_9(void),  irq64_10(void), irq64_11(void);
extern void irq64_12(void), irq64_13(void), irq64_14(void), irq64_15(void);

extern void isr64_default(void);

// M+11A: LAPIC spurious-vector (0xFF) stub -- counter only, NO EOI.
extern void isr64_spurious(void);
// M+11A: generic dynamic-vector stubs; entry [v - 0x30] for vectors
// 0x30..0xEE (0 for 0x80, the syscall gate).
extern const uint64_t irq64_vec_stub_table[0xEF - 0x30];

// Milestone 24: reusable "resume a suspended or brand-new process"
// target -- see kernel/isr64.asm and kernel/process64.c. Never called
// directly from C; only ever reached via context_switch64's `ret`
// popping this address off a process's own kernel stack. Declared here
// solely so kernel/process64.c can take its address.
extern void process64_resume_trapframe(void);

// Syscall gate (vector 128 / int 0x80). DPL=3 in its IDT entry lets ring3
// invoke it directly; see kernel/idt64.c and kernel/interrupt64.c.
extern void isr64_128(void);

// C-side dispatchers called from the asm stubs with rdi = &trapframe64_t.
void isr64_dispatch(trapframe64_t* tf);
void irq64_dispatch(trapframe64_t* tf);

#endif // ISR64_H
