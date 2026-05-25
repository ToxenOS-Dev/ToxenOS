#include <stdint.h>
#include "../include/pic.h"

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait()
{
    outb(0x80, 0);  // writing to unused port causes a small delay
}

void pic_remap()
{
    // save masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    // ICW1: start initialization sequence
    outb(PIC1_COMMAND, 0x11); io_wait();
    outb(PIC2_COMMAND, 0x11); io_wait();

    // ICW2: set vector offsets
    outb(PIC1_DATA, 0x20); io_wait();  // master starts at 32
    outb(PIC2_DATA, 0x28); io_wait();  // slave starts at 40

    // ICW3: tell master/slave about each other
    outb(PIC1_DATA, 0x04); io_wait();  // master: slave on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();  // slave: cascade identity

    // ICW4: set 8086 mode
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    // Do NOT restore original masks — on UEFI systems they are 0xFF (all masked)
    // because the IOAPIC handles routing.  We must unmask the IRQs we use.
    outb(PIC1_DATA, 0x00);  // unmask all master IRQs (0–7)
    outb(PIC2_DATA, 0xFF);  // keep slave IRQs masked (we don't use IRQ 8–15)
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);  // tell slave too if IRQ8-15

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (1 << irq));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & ~(1 << irq));
}