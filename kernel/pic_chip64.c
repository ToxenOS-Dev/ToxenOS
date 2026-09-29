// kernel/pic_chip64.c -- M+11A: the 8259 as an irq64_chip.
#include "../include/pic_chip64.h"
#include "../include/pic.h"

pic_spur_t pic_chip_classify(int hwirq, uint8_t isr_master, uint8_t isr_slave) {
    if (hwirq == 7)  return (isr_master & 0x80) ? PIC_SPUR_NONE : PIC_SPUR_NO_EOI;
    if (hwirq == 15) return (isr_slave  & 0x80) ? PIC_SPUR_NONE : PIC_SPUR_MASTER_EOI;
    return PIC_SPUR_NONE;
}

static void pic_c_mask(irq64_desc_t* d)   { pic_mask((uint8_t)d->hwirq); }
static void pic_c_unmask(irq64_desc_t* d) {
    // A slave line only reaches the CPU through the cascade input (IRQ2).
    if (d->hwirq >= 8) pic_unmask(2);
    pic_unmask((uint8_t)d->hwirq);
}
static void pic_c_eoi(irq64_desc_t* d)    { pic_send_eoi((uint8_t)d->hwirq); }

static int pic_c_spurious(irq64_desc_t* d) {
    if (d->hwirq != 7 && d->hwirq != 15) return 0;
    pic_spur_t s = pic_chip_classify((int)d->hwirq, pic_read_isr(0), pic_read_isr(1));
    if (s == PIC_SPUR_MASTER_EOI) pic_send_eoi_master();
    return s != PIC_SPUR_NONE;
}

const irq64_chip_t pic_chip64 = { "8259", pic_c_mask, pic_c_unmask, pic_c_eoi, pic_c_spurious, IRQ64_SRC_PIC, 0 };
