// kernel/interrupt64.c — Milestone 2 C-side exception dispatch/reporting.
// idt64.c only builds the gate table; this file interprets what actually
// faulted. Page fault (vector 14) is handled here too, deliberately NOT
// special-cased in kernel/isr64.asm the way the 32-bit isr14 stub is:
// every 64-bit ISR already builds a full trapframe64_t and hands this
// file one pointer, so CR2 can simply be read at the top of the page
// fault handler instead of being smuggled through bespoke asm.
#include <stdint.h>
#include "../include/isr64.h"
#include "../include/klog.h"
#include "../include/syscall64.h"

static const char* exception_name(uint64_t vector)
{
    static const char* names[32] = {
        "Divide By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
        "Stack Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 Floating Point", "Alignment Check", "Machine Check", "SIMD Floating Point",
        "Virtualization", "Control Protection", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Hypervisor Injection",
        "VMM Communication", "Security Exception", "Reserved", "Reserved"
    };
    if (vector < 32) return names[vector];
    return "Unknown vector (no explicit stub)";
}

static void hex64_to_str(uint64_t val, char* out)
{
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) { out[2 + (15 - i)] = h[val & 0xF]; val >>= 4; }
    out[18] = 0;
}

static void dec_to_str(uint64_t val, char* out)
{
    char tmp[24];
    int n = 0;
    if (val == 0) { tmp[n++] = '0'; }
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static void kv(const char* label, uint64_t val)
{
    char hex[19];
    hex64_to_str(val, hex);
    klog(label);
    klog(hex);
    klog("\n");
}

static void print_report(trapframe64_t* tf)
{
    char numbuf[24];
    dec_to_str(tf->vector, numbuf);
    klog("\n*** EXCEPTION ");
    klog(numbuf);
    klog(" (");
    klog(exception_name(tf->vector));
    klog(") ***\n");
    kv("  RIP:        ", tf->rip);
    kv("  CS:         ", tf->cs);
    kv("  RFLAGS:     ", tf->rflags);
    kv("  RSP:        ", tf->rsp);
    kv("  SS:         ", tf->ss);
    kv("  error_code: ", tf->error_code);

    // CS bits 0-1 are the CPL the CPU was running at when this trap
    // landed (the RPL of the saved CS, which the CPU always sets equal
    // to CPL on any exception/interrupt) -- not to be confused with the
    // *current* CPL while this handler runs, which is always 0.
    uint64_t cpl = tf->cs & 3;
    klog("  CPL:        ");
    klog(cpl == 3 ? "3 (ring3/user)\n" : "0 (ring0/kernel)\n");
}

static void halt_forever(void)
{
    klog("*** halting ***\n");
    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
}

// error_code bit layout matches the 32-bit page_fault_handler:
// bit0=present, bit1=write, bit2=user, bit4=instruction fetch.
static void page_fault64_handler(trapframe64_t* tf)
{
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    print_report(tf);
    kv("  CR2 (fault address): ", cr2);

    klog(tf->error_code & 1 ? "  protection violation" : "  page not present");
    klog(tf->error_code & 2 ? ", write" : ", read");
    klog(tf->error_code & 4 ? ", user\n" : ", kernel\n");
    if (tf->error_code & 16) klog("  (instruction fetch)\n");

    halt_forever();
}

void isr64_dispatch(trapframe64_t* tf)
{
    if (tf->vector == 14)  { page_fault64_handler(tf); return; }
    if (tf->vector == 128) { syscall64_dispatch(tf); return; }

    print_report(tf);

    // Vector 3 (#BP) is the one exception this milestone treats as
    // resumable: its saved RIP already points past the int3 byte, so
    // returning through the stub's normal epilogue resumes execution
    // cleanly. Every other vector (including ud2/#UD, whose saved RIP
    // points AT the faulting instruction with no architectural "skip
    // past it") halts — matching the milestone's "catch and halt
    // cleanly" success bar.
    if (tf->vector == 3) {
        klog("  (breakpoint -- resuming)\n");
        return;
    }

    halt_forever();
}
