#include <stdint.h>
#include "../include/process.h"
#include "../include/mm.h"
#include "../include/vga.h" 
#include "../include/paging.h"

static process_t  processes[MAX_PROCESSES];
static int        current_pid = 0;
static int        process_count = 0;

extern void process_iret_trampoline();

extern void context_switch(uint32_t* old_esp, uint32_t* new_esp);

static void copy_string(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static void process_trampoline()
{
    // get the entry point from the process table
    void (*entry)() = (void(*)())processes[current_pid].regs.eip;
    __asm__ volatile("sti");
    entry();
    while (1) __asm__ volatile("hlt"); 
}

void process_init()
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        processes[i].state = PROCESS_DEAD;

    // create kernel process (pid 0 = the current running kernel)
    processes[0].pid   = 0;
    processes[0].state = PROCESS_RUNNING;
    processes[0].page_directory = 0;
    copy_string(processes[0].name, "kernel", 32);
    processes[0].stack = 0;  // kernel uses its own stack from boot.asm
    process_count = 1;

    // save kernel ESP so we can switch back to it
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    processes[0].regs.esp = esp;

    process_count = 1;
}

int process_create(const char* name, void (*entry)())
{
    // find a free slot
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++)
    {
        if (processes[i].state == PROCESS_DEAD)
        {
            slot = i;
            break;
        }
    }

    if (slot == -1) return -1;  // no free slots

    process_t* p = &processes[slot];

    p->pid   = slot;
    p->state = PROCESS_READY;
    copy_string(p->name, name, 32);

    // allocate stack
    p->stack = (uint8_t*)kmalloc(PROCESS_STACK_SIZE);
    p->page_directory = paging_create_directory();

    // set up stack so it looks like an interrupt just happened
    // stack grows downward so start at top
    uint32_t* stack_top = (uint32_t*)(p->stack + PROCESS_STACK_SIZE);

    // iret frame
    *(--stack_top) = 0x00000202;            // eflags: IF=1 (interrupts enabled)
    *(--stack_top) = 0x08;                  // cs
    *(--stack_top) = (uint32_t)entry;       // eip

    // fake return address for the ret in context_switch to consume
    *(--stack_top) = (uint32_t)process_iret_trampoline;

    *(--stack_top) = 0;  // ebp
    *(--stack_top) = 0;  // edi
    *(--stack_top) = 0;  // esi
    *(--stack_top) = 0;  // ebx

    p->regs.esp = (uint32_t)stack_top;
    p->regs.eip = (uint32_t)entry;

    process_count++;
    return slot;
}

void process_exit()
{
    if (current_pid == 0) return;

    processes[current_pid].state = PROCESS_DEAD;
    process_count--;

    // find next ready process
    int next = -1;
    for (int i = 1; i < MAX_PROCESSES; i++)
    {
        if (processes[i].state == PROCESS_READY)
        {
            next = i;
            break;
        }
    }

    if (next == -1)
    {
        // nothing to run, idle
        current_pid = 0;
        processes[0].state = PROCESS_RUNNING;
        paging_switch(kernel_directory);

        __asm__ volatile(
            "mov %0, %%esp\n"
            "sti\n"
            "1: hlt\n"
            "jmp 1b\n"
            :
            : "r"(processes[0].regs.esp)
        );
    }

    processes[next].state = PROCESS_RUNNING;
    int old = current_pid;
    current_pid = next;

    if (processes[next].page_directory)
        paging_switch(processes[next].page_directory);

    context_switch(&processes[old].regs.esp, &processes[next].regs.esp);
}

process_t* process_current()
{
    return &processes[current_pid];
}


void scheduler()
{
    int next = current_pid;

    for (int i = 1; i <= MAX_PROCESSES; i++)
    {
        int candidate = (current_pid + i) % MAX_PROCESSES;
        if (processes[candidate].state == PROCESS_READY ||
            processes[candidate].state == PROCESS_RUNNING)
        {
            next = candidate;
            break;
        }
    }

    if (next == current_pid)
    {
        // if nothing ready, go back to kernel process (pid 0)
        if (current_pid != 0 && processes[0].state != PROCESS_DEAD)
            next = 0;
        else
            return;
    }

    processes[current_pid].state = PROCESS_READY;
    processes[next].state = PROCESS_RUNNING;

    int old = current_pid;
    current_pid = next;

    if (processes[next].page_directory)
        paging_switch(processes[next].page_directory);

    context_switch(&processes[old].regs.esp, &processes[next].regs.esp);
}