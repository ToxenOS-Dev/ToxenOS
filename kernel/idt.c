// ToxenOS/kernel/idt.c
#include <stdint.h>
#include "../include/idt.h"
#include "../include/process.h"
#include "../include/vga.h"
#include "../include/paging.h"
#include "../include/memmap.h"

static const char* exception_messages[] = {
    "Divide By Zero",           // 0
    "Debug",                    // 1
    "Non Maskable Interrupt",   // 2
    "Breakpoint",               // 3
    "Overflow",                 // 4
    "Bound Range Exceeded",     // 5
    "Invalid Opcode",           // 6
    "Device Not Available",     // 7
    "Double Fault",             // 8
    "Coprocessor Segment Overrun", // 9
    "Invalid TSS",              // 10
    "Segment Not Present",      // 11
    "Stack Segment Fault",      // 12
    "General Protection Fault", // 13
    "Page Fault",               // 14
    "Reserved",                 // 15
    "x87 Floating Point",       // 16
    "Alignment Check",          // 17
    "Machine Check",            // 18
    "SIMD Floating Point",      // 19
    "Virtualization",           // 20
    "Control Protection",       // 21
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection",     // 28
    "VMM Communication",        // 29
    "Security Exception",       // 30
    "Reserved"                  // 31
};

static void pf_print_hex(uint32_t val)
{
    const char* h = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    print(buf);
}

// ── General exception handler ─────────────────────────────────────────────────
// Called for all exceptions except page fault (ISR 14).
// error_code is 0 for exceptions that don't push one.
void exception_handler(int interrupt, uint32_t error_code)
{
    process_t* p = process_current();
    int is_kernel = (p->pid == 0);

    if (is_kernel) {
        // Kernel fault — full panic, nothing we can do
        __asm__("cli");
        set_color(0x0C);
        print("\n\n*** KERNEL PANIC ***\n");
        print(exception_messages[interrupt & 0x1F]);
        print("  error="); pf_print_hex(error_code);
        print("\n");
        set_color(0x07);
        while (1) __asm__("hlt");
    } else {
        // User process fault — kill it and return to scheduler
        set_color(0x0C);
        print("\nProcess fault: ");
        print(exception_messages[interrupt & 0x1F]);
        print(" (pid=");
        pf_print_hex(p->pid);
        print(" err=");
        pf_print_hex(error_code);
        print(")\n");
        set_color(0x07);
        process_exit();
        // process_exit() does not return
    }
}

// ── Page fault handler ────────────────────────────────────────────────────────
// error_code bits: 0=present, 1=write, 2=user, 3=reserved, 4=ifetch
// cr2 = the virtual address that was accessed
void page_fault_handler(uint32_t error_code, uint32_t cr2)
{
    process_t* p   = process_current();
    int present    = error_code & 1;
    int write      = (error_code >> 1) & 1;
    int user       = (error_code >> 2) & 1;
    int is_kernel  = (p->pid == 0);

    if (is_kernel) {
        __asm__("cli");
        set_color(0x0C);
        print("\n\n*** KERNEL PAGE FAULT ***\n");
        print("addr="); pf_print_hex(cr2);
        print(present ? "  [protection]" : "  [not mapped]");
        print(write   ? "  [write]"      : "  [read]");
        print("\n");
        set_color(0x07);
        while (1) __asm__("hlt");
    } else {
        set_color(0x0C);
        // Check if this is likely a stack overflow (hit the guard page)
        uint32_t guard = USER_STACK_TOP - (USER_STACK_PAGES + 1) * PAGE_SIZE;
        if (cr2 >= guard && cr2 < guard + PAGE_SIZE)
            print("\nStack overflow in '");
        else
            print("\nSegfault in '");
        print(p->name);
        print("'  addr="); pf_print_hex(cr2);
        print(present ? " [protection]" : " [not mapped]");
        print(write   ? " [write]"      : " [read]");
        print(user    ? " [user]\n"     : " [kernel]\n");
        set_color(0x07);
        process_exit();
    }
}

// ── IDT setup ─────────────────────────────────────────────────────────────────

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  zero;
    uint8_t  type_attr;
    uint16_t offset_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

extern void isr_stub();

void idt_set_gate(int n, uint32_t handler)
{
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;
    idt[n].zero        = 0;
    idt[n].type_attr   = 0x8E;  // present, ring 0, interrupt gate
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

void idt_set_gate_user(int n, uint32_t handler)
{
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = 0x08;
    idt[n].zero        = 0;
    idt[n].type_attr   = 0xEE;  // present, ring 3, interrupt gate
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

extern void isr0(),  isr1(),  isr2(),  isr3(),  isr4(),  isr5(),  isr6(),  isr7();
extern void isr8(),  isr9(),  isr10(), isr11(), isr12(), isr13(), isr14(), isr15();
extern void isr16(), isr17(), isr18(), isr19(), isr20(), isr21(), isr22(), isr23();
extern void isr24(), isr25(), isr26(), isr27(), isr28(), isr29(), isr30(), isr31();
extern void irq0(),  irq1(),  irq2(),  irq3(),  irq4(),  irq5(),  irq6(),  irq7();
extern void irq8(),  irq9(),  irq10(), irq11(), irq12(), irq13(), irq14(), irq15();

void idt_init()
{
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    for (int i = 0; i < 256; i++)
        idt_set_gate(i, (uint32_t)isr_stub);

    idt_set_gate(0,  (uint32_t)isr0);
    idt_set_gate(1,  (uint32_t)isr1);
    idt_set_gate(2,  (uint32_t)isr2);
    idt_set_gate(3,  (uint32_t)isr3);
    idt_set_gate(4,  (uint32_t)isr4);
    idt_set_gate(5,  (uint32_t)isr5);
    idt_set_gate(6,  (uint32_t)isr6);
    idt_set_gate(7,  (uint32_t)isr7);
    idt_set_gate(8,  (uint32_t)isr8);
    idt_set_gate(9,  (uint32_t)isr9);
    idt_set_gate(10, (uint32_t)isr10);
    idt_set_gate(11, (uint32_t)isr11);
    idt_set_gate(12, (uint32_t)isr12);
    idt_set_gate(13, (uint32_t)isr13);
    idt_set_gate(14, (uint32_t)isr14);
    idt_set_gate(15, (uint32_t)isr15);
    idt_set_gate(16, (uint32_t)isr16);
    idt_set_gate(17, (uint32_t)isr17);
    idt_set_gate(18, (uint32_t)isr18);
    idt_set_gate(19, (uint32_t)isr19);
    idt_set_gate(20, (uint32_t)isr20);
    idt_set_gate(21, (uint32_t)isr21);
    idt_set_gate(22, (uint32_t)isr22);
    idt_set_gate(23, (uint32_t)isr23);
    idt_set_gate(24, (uint32_t)isr24);
    idt_set_gate(25, (uint32_t)isr25);
    idt_set_gate(26, (uint32_t)isr26);
    idt_set_gate(27, (uint32_t)isr27);
    idt_set_gate(28, (uint32_t)isr28);
    idt_set_gate(29, (uint32_t)isr29);
    idt_set_gate(30, (uint32_t)isr30);
    idt_set_gate(31, (uint32_t)isr31);

    idt_set_gate(32, (uint32_t)irq0);
    idt_set_gate(33, (uint32_t)irq1);
    idt_set_gate(34, (uint32_t)irq2);
    idt_set_gate(35, (uint32_t)irq3);
    idt_set_gate(36, (uint32_t)irq4);
    idt_set_gate(37, (uint32_t)irq5);
    idt_set_gate(38, (uint32_t)irq6);
    idt_set_gate(39, (uint32_t)irq7);
    idt_set_gate(40, (uint32_t)irq8);
    idt_set_gate(41, (uint32_t)irq9);
    idt_set_gate(42, (uint32_t)irq10);
    idt_set_gate(43, (uint32_t)irq11);
    idt_set_gate(44, (uint32_t)irq12);
    idt_set_gate(45, (uint32_t)irq13);
    idt_set_gate(46, (uint32_t)irq14);
    idt_set_gate(47, (uint32_t)irq15);

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}
