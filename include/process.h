#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vfs.h"

#define KERNEL_STACK_SIZE  65536   // 64KB kernel stack per process
#define MAX_PROCESSES      16

typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_WAITING,   // blocked on event (wait queue or waiting_for pid)
    PROCESS_SLEEPING,  // sleeping until sleep_until ticks
    PROCESS_DEAD
} process_state_t;

typedef struct {
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip, eflags;
} registers_t;

typedef struct {
    uint32_t        pid;
    process_state_t state;
    int             waiting_for;
    uint32_t        sleep_until;
    int             tty;             // which TTY this process belongs to (-1 = none)
    int             stdin_fd;        // legacy: -1 = keyboard, >=0 = pipe
    int             stdout_fd;       // legacy: -1 = screen,   >=0 = pipe
    uint32_t        heap_end;
    fd_table_t      fds;             // per-process FD table
    registers_t     regs;
    uint8_t*        kernel_stack;
    uint32_t        user_stack;
    uint32_t*       page_directory;
    char            name[32];
    char            args[256];
    uint8_t         is_admin;        // 1 = elevated (can write to protected dirs)
} process_t;

extern process_t processes[MAX_PROCESSES];
void       process_init();
void       process_retire_kernel(); // mark PID 0 dead before jumping to ring 3
int        process_create(const char* name, void (*entry)());
int        process_create_elf(const char* name, uint8_t* elf_buf, uint32_t elf_size);
void       process_exit();
void       scheduler();
process_t* process_current();

#endif

int  sys_exec(const char* path);
int  sys_spawn(const char* path);
void sys_wait(int pid);
void sys_sleep(uint32_t ms);
uint32_t sys_sbrk(int32_t increment);
int  process_is_alive(int pid);
int  sys_spawn_tty(const char* path, int tty);
int  sys_spawn_tty_args(const char* path, int tty, const char* args);
process_t* process_get_by_pid(int pid);
