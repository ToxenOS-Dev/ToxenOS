// ToxenOS/kernel/ring3.c
// Drop the current execution context into ring 3 (user mode).
// The caller is responsible for ensuring the entry point and user stack
// are already mapped in the current page directory with PAGE_USER.
#include <stdint.h>
#include "../include/ring3.h"
#include "../include/tss.h"
#include "../include/paging.h"
#include "../include/process.h"

extern uint32_t stack_top;

void jump_to_ring3(void (*entry)(), uint32_t user_stack)
{
    // TSS must point to kernel stack so IRQs/syscalls have somewhere to go
    tss_set_kernel_stack((uint32_t)&stack_top);

    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"   // 0x23 = ring3 data segment (GDT index 4, RPL 3)
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        // Build iret frame: ss, esp, eflags, cs, eip
        "push $0x23\n"        // ss
        "push %1\n"           // esp (user stack)
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"  // set IF so interrupts are enabled in ring3
        "push %%eax\n"        // eflags
        "push $0x1B\n"        // cs = 0x1B (ring3 code segment, GDT index 3, RPL 3)
        "push %0\n"           // eip (entry point)
        "iret\n"
        :
        : "r"(entry), "r"(user_stack)
        : "eax"
    );
}
