#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

#define PROCESS_STACK_SIZE  16384   // 16KB instead of 8KB
#define MAX_PROCESSES       16

typedef enum
{
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_DEAD
} process_state_t;

typedef struct
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    uint32_t eip, eflags;
} registers_t;

typedef struct
{
    uint32_t        pid;
    process_state_t state;
    registers_t     regs;
    uint8_t*        stack;
    uint32_t*       page_directory;
    char            name[32];
} process_t;

void process_init();
int  process_create(const char* name, void (*entry)());
void process_exit();
void scheduler();
process_t* process_current();

#endif