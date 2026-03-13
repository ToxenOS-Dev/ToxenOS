#include <stdint.h>
#include "../include/ring3.h"
#include "../include/tss.h"
#include "../include/paging.h"
#include "../include/process.h"

extern uint32_t stack_top;

void jump_to_ring3(void (*entry)(), uint32_t user_stack)
{
    // map user stack
    extern uint32_t kernel_directory[];
    for (int i = 0; i < 4; i++)
    {
        paging_map(kernel_directory,
                   user_stack - 4096 + (i * 1024),
                   user_stack - 4096 + (i * 1024),
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    // map user code
    paging_map(kernel_directory,
               (uint32_t)entry & ~0xFFF,
               (uint32_t)entry & ~0xFFF,
               PAGE_PRESENT | PAGE_USER);

    // set TSS to point to KERNEL stack, not user stack
    tss_set_kernel_stack((uint32_t)&stack_top);

    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "push $0x23\n"
        "push %1\n"
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"
        "push %%eax\n"
        "push $0x1b\n"
        "push %0\n"
        "iret\n"
        :
        : "r"(entry), "r"(user_stack)
        : "eax"
    );
}