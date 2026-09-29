#ifndef PIC_H
#define PIC_H

#include <stdint.h>

#define PIC1_COMMAND    0x20
#define PIC1_DATA       0x21
#define PIC2_COMMAND    0xA0
#define PIC2_DATA       0xA1

#define PIC_EOI         0x20    // End Of Interrupt signal

void pic_remap();
void pic_send_eoi(uint8_t irq);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);

// M+11A (64-bit interrupt core only):
void    pic_mask_all(void);
uint8_t pic_get_mask(int slave);            // slave: 0 = master IMR, 1 = slave IMR
void    pic_set_mask(int slave, uint8_t mask);
uint8_t pic_read_isr(int slave);            // OCW3 ISR read
void    pic_send_eoi_master(void);

#endif