#include <stdint.h>
#include "../include/tss.h"
#include "../include/klog.h"
#include "../include/memmap.h"

static tss_t tss;

// gdt_base_phys is in .boot section (low address).
// We need to access it via its high-half virtual address.
extern uint32_t gdt_base;

void tss_init(uint32_t kernel_stack)
{
    uint8_t* p = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(tss_t); i++)
        p[i] = 0;

    tss.ss0  = 0x10;
    tss.esp0 = kernel_stack;

    uint32_t base  = (uint32_t)&tss;
    uint32_t limit = sizeof(tss_t) - 1;

    // gdt_base is in .boot at physical 0x1000xx.
    // Access it via high-half virtual address: phys + KERNEL_VIRT_BASE.
    // The value it contains is already a virtual address (gdt + KERNEL_VIRT_BASE).
    uint32_t gdt_base_virt = KPHYS_TO_VIRT((uint32_t)&gdt_base);
    uint32_t gdt_addr = *(uint32_t*)gdt_base_virt;
    uint8_t* entry = (uint8_t*)(gdt_addr + 40);

    entry[0] = limit & 0xFF;
    entry[1] = (limit >> 8) & 0xFF;
    entry[2] = base & 0xFF;
    entry[3] = (base >> 8) & 0xFF;
    entry[4] = (base >> 16) & 0xFF;
    entry[5] = 0x89;
    entry[6] = 0x00;
    entry[7] = (base >> 24) & 0xFF;

    klog("ltr\n");
    __asm__ volatile("ltr %0" : : "r"((uint16_t)0x28));
    klog("ltr done\n");
}

void tss_set_kernel_stack(uint32_t stack)
{
    tss.esp0 = stack;
}