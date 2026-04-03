#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define KERNEL_STACK_SIZE  16384   // 16KB kernel stack per process
#define MAX_PROCESSES      16

typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_WAITING,   // blocked in sys_wait() until waiting_for dies
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
    int             waiting_for;     // pid we're blocked on (-1 = none)
    int             stdin_fd;        // -1 = keyboard, >=0 = pipe read end
    int             stdout_fd;       // -1 = screen,   >=0 = pipe write end
    registers_t     regs;
    uint8_t*        kernel_stack;
    uint32_t        user_stack;
    uint32_t*       page_directory;
    char            name[32];
    char            args[256];
} process_t;

extern process_t processes[MAX_PROCESSES];
void       process_init();
int        process_create(const char* name, void (*entry)());
int        process_create_elf(const char* name, uint8_t* elf_buf, uint32_t elf_size);
void       process_exit();
void       scheduler();
process_t* process_current();

#endif

int  sys_exec(const char* path);
int  sys_spawn(const char* path);
void sys_wait(int pid);
int  process_is_alive(int pid);
int  sys_spawn_tty(const char* path, int tty);
int sys_spawn_tty_args(const char* path, int tty, const char* args);
