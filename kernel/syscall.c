#include <stdint.h>

// ── Kernel log buffer ────────────────────────────────────────────────────────
#define KLOG_SIZE 4096
static char klog_buf[KLOG_SIZE];
static int  klog_pos = 0;

void klog(const char* msg) {
    for (int i = 0; msg[i] && klog_pos < KLOG_SIZE - 1; i++)
        klog_buf[klog_pos++] = msg[i];
    klog_buf[klog_pos] = 0;
}
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
            uint32_t size = 0;
            if (vfs_stat((const char*)ebx, &size) < 0) return -1;
            return (int)size;
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

        case SYS_BMSG:
        {
            char* buf  = (char*)ebx;
            uint32_t sz = (uint32_t)ecx;
            uint32_t len = 0;
            while (klog_buf[len] && len < sz - 1) {
                buf[len] = klog_buf[len];
                len++;
            }
            buf[len] = 0;
            return (int)len;
        }

        case SYS_PROC_LIST:
        {
            // Returns process info as packed array of: pid(4) + state(4) + name(32) = 40 bytes each
            uint8_t* buf  = (uint8_t*)ebx;
            uint32_t size = (uint32_t)ecx;
            uint32_t written = 0;
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processes[i].state == PROCESS_DEAD) continue;
                if (written + 40 > size) break;
                // pid
                buf[written+0] = processes[i].pid & 0xFF;
                buf[written+1] = (processes[i].pid >> 8) & 0xFF;
                buf[written+2] = 0; buf[written+3] = 0;
                // state
                buf[written+4] = (uint8_t)processes[i].state;
                buf[written+5] = 0; buf[written+6] = 0; buf[written+7] = 0;
                // name (32 bytes)
                for (int j = 0; j < 32; j++)
                    buf[written+8+j] = processes[i].name[j];
                written += 40;
            }
            return (int)written;
        }

        case SYS_KILL:
        {
            int pid = (int)ebx;
            if (pid <= 0) return -1;  // can't kill kernel
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if ((int)processes[i].pid == pid && processes[i].state != PROCESS_DEAD) {
                    processes[i].state = PROCESS_DEAD;
                    return 0;
                }
            }
            return -1;
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
