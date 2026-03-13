#include <stdint.h>
#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/process.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vga.h"

// this gets called from assembly with all registers saved
uint32_t __attribute__((cdecl)) syscall_handler(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx)
{
    switch (eax)
    {
        case SYS_EXIT:
            sys_exit((int)ebx);
            return 0;

        case SYS_PRINT:
            sys_print((const char*)ebx);
            return 0;

        case SYS_GETCHAR:
            if (keyboard_available())
                return keyboard_getchar();
            return 0;

        case SYS_SETCOLOR:
            set_color((uint8_t)ebx);
            return 0;

        case SYS_ERASE:
            erase_char();
            return 0;

        case SYS_YIELD:
            __asm__ volatile("hlt");
            return 0;

        case SYS_GETPID:
            return 0;

        default:
            print("Unknown syscall\n");
            return -1;
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
    idt_set_gate_user(0x80, (uint32_t)isr128);
}