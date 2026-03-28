#include <stdint.h>
#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/process.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"
#include "../include/tty.h"
#include "../include/fbterm.h"

extern uint8_t _binary_build_user_shell_elf_start[];
extern uint8_t _binary_build_user_shell_elf_end[];

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

        case SYS_CLEAR:
            clear_screen();
            return 0;

        case SYS_REBOOT:
            __asm__ volatile("movb $0xFE,%%al; outb %%al,$0x64":::"eax");
            return 0;

        case SYS_SHUTDOWN:
            __asm__ volatile("movw $0x2000,%%ax; movw $0x604,%%dx; outw %%ax,%%dx":::"eax","edx");
            return 0;

        case SYS_READDIR:
            return vfs_readdir((const char*)ebx, (char*)ecx, edx);

        case SYS_OPEN:
            return vfs_open((const char*)ebx, ecx);

        case SYS_READ:
            return vfs_read(ebx, (uint8_t*)ecx, edx);

        case SYS_WRITE:
            return vfs_write(ebx, (const uint8_t*)ecx, edx);

        case SYS_CLOSE:
            return vfs_close(ebx);

        case SYS_STAT:
        {
            uint32_t size;
            return vfs_stat((const char*)ebx, &size);
        }

        case SYS_ISDIR:
            return vfs_isdir((const char*)ebx);

        case SYS_MKDIR:
            return vfs_mkdir((const char*)ebx);

        case SYS_REMOVE:
            return vfs_remove((const char*)ebx);

        case SYS_YIELD:
            scheduler();
            return 0;

        case SYS_GETPID:
            return process_current()->pid;

        case SYS_GET_TTY:
            return tty_current();

        case SYS_MY_TTY:
            return tty_for_pid[process_current()->pid];

        case SYS_EXEC:
            return sys_exec((const char*)ebx);

        case SYS_SPAWN:
            return sys_spawn((const char*)ebx);

        case SYS_WAIT:
            sys_wait((int)ebx);
            return 0;

        case SYS_SPAWN_TTY:
            return sys_spawn_tty((const char*)ebx, (int)ecx);

        case 26:  // SYS_GET_ARGS
        {
            char* buf = (char*)ebx;
            const char* src = process_current()->args;
            int i = 0;
            while (src[i] && i < 255) { buf[i] = src[i]; i++; }
            buf[i] = 0;
            return i;
        }

        case 27:  // SYS_SPAWN_ARGS
            return sys_spawn_tty_args((const char*)ebx, (int)ecx, (const char*)edx);

        case 28:  // SYS_IS_ALIVE
            return process_is_alive((int)ebx);

        case 29:  // SYS_KEYAVAIL
            return keyboard_available();

        case SYS_SPAWN_EMBEDDED:
        {
            uint8_t* buf  = _binary_build_user_shell_elf_start;
            uint32_t size = (uint32_t)(_binary_build_user_shell_elf_end
                                      - _binary_build_user_shell_elf_start);
            int tty = (int)ebx;
            int pid = process_create_elf("shell", buf, size);
            if (pid < 0) return -1;
            tty_assign_pid(pid, tty);
            fbterm_pid_tty[pid] = tty;
            return pid;
        }

        default:
            print("Unknown syscall\n");
            return -1;
    }
}

void sys_exit(int code)   { process_exit(); }
void sys_print(const char* str) { print(str); }
char sys_getchar()        { return keyboard_getchar(); }
int  sys_getpid()         { return process_current()->pid; }

void syscall_init()
{
    extern void isr128();
    extern void idt_set_gate(int n, uint32_t handler);
    idt_set_gate_user(0x80, (uint32_t)isr128);
}
