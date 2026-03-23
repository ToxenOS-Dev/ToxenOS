#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define KERNEL_STACK_SIZE  16384   // 16KB kernel stack per process
#define MAX_PROCESSES      16

typedef enum {
    PROCESS_READY,
    PROCESS_RUNNING,
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
    registers_t     regs;
    uint8_t*        kernel_stack;    // kernel-mode stack (for syscalls/IRQs)
    uint32_t        user_stack;      // user-mode stack virtual address
    uint32_t*       page_directory;  // this process's page directory
    char            name[32];
} process_t;

void       process_init();
int        process_create(const char* name, void (*entry)());
int        process_create_elf(const char* name, uint8_t* elf_buf, uint32_t elf_size);
void       process_exit();
void       scheduler();
process_t* process_current();

#endif
