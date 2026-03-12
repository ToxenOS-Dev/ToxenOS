#include <stdint.h>
#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/process.h"
#include "../include/vga.h"
#include "../include/keyboard.h"

// this gets called from assembly with all registers saved
void syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    switch (eax)
    {
        case SYS_EXIT:
            sys_exit((int)ebx);
            break;

        case SYS_PRINT:
            sys_print((const char*)ebx);
            break;

        case SYS_GETCHAR:
            // return value goes back in eax
            // handled specially in assembly
            break;

        case SYS_GETPID:
            break;

        default:
            print("Unknown syscall: ");
            break;
    }
}

void sys_exit(int code)
{
    process_exit();
}

void sys_print(const char* str)
{
    print(str);
}

char sys_getchar()
{
    return keyboard_getchar();
}

int sys_getpid()
{
    return process_current()->pid;
}

void syscall_init()
{
    // register int 0x80 in IDT
    // we'll wire this up in isr.asm
    extern void isr128();
    
    // reuse idt_set_gate — need to expose it
    // for now declare it extern
    extern void idt_set_gate(int n, uint32_t handler);
    idt_set_gate(0x80, (uint32_t)isr128);
}