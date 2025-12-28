#include "cpu/interrupts/idt.h"
#include "devices/display/vga.h"
#include "devices/input/keyboard.h"


/* IDT structures */
struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

extern void isr_timer(void);
extern void isr_keyboard(void);

static struct idt_entry idt[256];
static struct idt_ptr idtp;

/* Port I/O */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low  = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel       = sel;
    idt[num].always0   = 0;
    idt[num].flags     = flags;
}

/* PIC remap */
static void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    /* Enable IRQ0 (timer) + IRQ1 (keyboard) */
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

/* Timer handler (IRQ0) */
void timer_handler(void) {
    vga_blink_cursor(cursor_x, cursor_y);
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    pic_remap();

    idt_set_gate(0x20, (uint32_t)isr_timer,    0x08, 0x8E);
    idt_set_gate(0x21, (uint32_t)isr_keyboard, 0x08, 0x8E);

    __asm__ volatile ("lidt %0" : : "m"(idtp));
    __asm__ volatile ("sti");
}
