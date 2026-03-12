#include <stdint.h>
#include "../include/tss.h"

static tss_t tss;

extern uint32_t tss_entry;  // points to TSS slot in GDT

void tss_init(uint32_t kernel_stack)
{
    uint8_t* p = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss_t); i++)
        p[i] = 0;

    tss.ss0  = 0x10;
    tss.esp0 = kernel_stack;

    uint32_t base  = (uint32_t)&tss;
    uint32_t limit = sizeof(tss_t) - 1;

    // write TSS descriptor directly into GDT slot
    uint8_t* entry = (uint8_t*)(*(uint32_t*)&tss_entry);

    entry[0] = limit & 0xFF;
    entry[1] = (limit >> 8) & 0xFF;
    entry[2] = base & 0xFF;
    entry[3] = (base >> 8) & 0xFF;
    entry[4] = (base >> 16) & 0xFF;
    entry[5] = 0x89;
    entry[6] = 0x00;
    entry[7] = (base >> 24) & 0xFF;

    __asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));
}

void tss_set_kernel_stack(uint32_t stack)
{
    tss.esp0 = stack;
}