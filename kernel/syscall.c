#include <stdint.h>
#include "../include/syscall.h"
#include "../include/idt.h"
#include "../include/process.h"
#include "../include/vga.h"
#include "../include/keyboard.h"
#include "../include/vga.h"
#include "../include/vfs.h"
#include "../include/tty.h"
#include "../include/process.h"
#include "../include/fbterm.h"

extern uint8_t _binary_build_user_shell_elf_start[];
extern uint8_t _binary_build_user_shell_elf_end[];

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

        case SYS_CLEAR:
            clear_screen();
            return 0;

        case SYS_REBOOT:
            __asm__ volatile(
                "movb $0xFE, %%al\n"
                "outb %%al, $0x64\n"
                ::: "eax"
            );
            return 0;

        case SYS_SHUTDOWN:
            __asm__ volatile(
                "movw $0x2000, %%ax\n"
                "movw $0x604, %%dx\n"
                "outw %%ax, %%dx\n"
                ::: "eax", "edx"
            );
            return 0;

        case SYS_READDIR:
            // ebx = path, ecx = out buffer, edx = index
            return vfs_readdir((const char*)ebx, (char*)ecx, edx);

        case SYS_OPEN:
            // ebx = path, ecx = flags
            return vfs_open((const char*)ebx, ecx);

        case SYS_READ:
            // ebx = fd, ecx = buf, edx = size
            return vfs_read(ebx, (uint8_t*)ecx, edx);

        case SYS_WRITE:
            // ebx = fd, ecx = buf, edx = size
            return vfs_write(ebx, (const uint8_t*)ecx, edx);

        case SYS_CLOSE:
            // ebx = fd
            return vfs_close(ebx);

        case SYS_STAT:
        {
            uint32_t size;
            return vfs_stat((const char*)ebx, &size);
        }

        case SYS_ISDIR:
        {
            // check if path is a directory
            int mount_idx = -1;
            // reuse vfs_stat but check inode type
            return vfs_isdir((const char*)ebx);
        }
            
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

        case SYS_GET_ARGS:
        {
            // copy current process args into user buffer (ebx)
            char* buf = (char*)ebx;
            const char* src = process_current()->args;
            int i = 0;
            while (src[i] && i < 255) { buf[i] = src[i]; i++; }
            buf[i] = 0;
            return i;
        }

        case SYS_SPAWN_ARGS:
            // ebx=path, ecx=tty, edx=args
            return sys_spawn_tty_args((const char*)ebx, (int)ecx, (const char*)edx);

        case SYS_EXEC_CMD:
            return sys_exec_cmd((const char*)ebx, (int)ecx, (const char*)edx);

        case SYS_SPAWN_EMBEDDED:
        {
            // spawn the embedded shell ELF on the given TTY
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