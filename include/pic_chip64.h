#ifndef PIC_CHIP64_H
#define PIC_CHIP64_H

// M+11A: 8259 irq64_chip + pure spurious classification.
#include "irq64.h"

typedef enum {
    PIC_SPUR_NONE = 0,       // genuine interrupt
    PIC_SPUR_NO_EOI,         // spurious master IRQ7: send NO EOI
    PIC_SPUR_MASTER_EOI,     // spurious slave IRQ15: EOI the MASTER only (cascade was real)
} pic_spur_t;

// Pure: given the ISR contents read via OCW3, is a delivery on `hwirq`
// spurious? Only lines 7 and 15 can be.
pic_spur_t pic_chip_classify(int hwirq, uint8_t isr_master, uint8_t isr_slave);

extern const irq64_chip_t pic_chip64;

#endif // PIC_CHIP64_H
