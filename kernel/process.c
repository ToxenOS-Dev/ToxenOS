// ToxenOS/kernel/process.c
#include <stdint.h>
#include "../include/process.h"
#include "../include/mm.h"
#include "../include/paging.h"
#include "../include/vga.h"
#include "../include/tss.h"

static process_t processes[MAX_PROCESSES];
static int       current_pid   = 0;
static int       process_count = 0;

extern void context_switch(uint32_t* old_esp, uint32_t* new_esp);
extern void process_iret_trampoline();
extern uint32_t stack_top;  // kernel stack top from boot.asm

static void copy_str(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max-1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void process_init()
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        processes[i].state = PROCESS_DEAD;

    processes[0].pid            = 0;
    processes[0].state          = PROCESS_RUNNING;
    processes[0].page_directory = kernel_directory;
    processes[0].user_stack     = 0;
    copy_str(processes[0].name, "kernel", 32);

    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    processes[0].regs.esp = esp;

    process_count = 1;
}

// Load ELF segments into a process's own page directory.
// Each PT_LOAD segment gets fresh physical pages — no sharing.
// Returns entry point, or 0 on failure.
static uint32_t load_elf_into_dir(uint32_t* dir,
                                   uint8_t* elf_buf,
                                   uint32_t elf_size)
{
    // Validate ELF header
    if (elf_size < 52) return 0;
    uint32_t magic = *(uint32_t*)elf_buf;
    if (magic != 0x464C457F) return 0;

    uint32_t entry    = *(uint32_t*)(elf_buf + 24);
    uint32_t phoff    = *(uint32_t*)(elf_buf + 28);
    uint16_t phentsize = *(uint16_t*)(elf_buf + 42);
    uint16_t phnum    = *(uint16_t*)(elf_buf + 44);

    for (int i = 0; i < phnum; i++)
    {
        uint8_t* ph = elf_buf + phoff + i * phentsize;

        uint32_t type   = *(uint32_t*)(ph + 0);
        uint32_t offset = *(uint32_t*)(ph + 4);
        uint32_t vaddr  = *(uint32_t*)(ph + 8);
        uint32_t filesz = *(uint32_t*)(ph + 16);
        uint32_t memsz  = *(uint32_t*)(ph + 20);
        uint32_t flags  = *(uint32_t*)(ph + 24);

        if (type != 1) continue;  // PT_LOAD = 1
        if (memsz == 0) continue;

        uint32_t pf = PAGE_PRESENT | PAGE_USER;
        if (flags & 2) pf |= PAGE_WRITABLE;  // PF_W

        // Map each page of this segment
        uint32_t page_start = vaddr & ~0xFFF;
        uint32_t page_end   = (vaddr + memsz + 0xFFF) & ~0xFFF;

        for (uint32_t va = page_start; va < page_end; va += PAGE_SIZE)
        {
            uint32_t phys = paging_alloc_page();
            if (!phys) return 0;

            paging_map(dir, va, phys, pf);

            // We need to temporarily access this physical page to copy data.
            // Since we have identity mapping for all kernel memory, phys == virt
            // for pages allocated by kmalloc (which lives in kernel heap < 1GB).
            uint8_t* dst = (uint8_t*)phys;

            // How many bytes of file data go into this page?
            uint32_t page_offset = va > vaddr ? va - vaddr : 0;
            uint32_t file_start  = vaddr + page_offset > vaddr ? page_offset : 0;

            // Zero the whole page first (handles BSS)
            for (int j = 0; j < (int)PAGE_SIZE; j++) dst[j] = 0;

            // Copy file data that falls within this page
            {
                uint32_t page_vstart = va;
                uint32_t seg_vend    = vaddr + filesz;
                for (uint32_t b = 0; b < PAGE_SIZE; b++) {
                    uint32_t cur_va = page_vstart + b;
                    if (cur_va < vaddr)    continue;
                    if (cur_va >= seg_vend) break;
                    dst[b] = elf_buf[offset + (cur_va - vaddr)];
                }
            }
        }
    }

    return entry;
}

// Create a new isolated user process.
// elf_buf/elf_size: the ELF binary to load.
// Returns pid on success, -1 on failure.
int process_create_elf(const char* name, uint8_t* elf_buf, uint32_t elf_size)
{
    // Find free slot
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++)
        if (processes[i].state == PROCESS_DEAD) { slot = i; break; }
    if (slot == -1) return -1;

    process_t* p = &processes[slot];
    p->pid   = slot;
    p->state = PROCESS_READY;
    copy_str(p->name, name, 32);

    // Create an isolated page directory for this process
    p->page_directory = paging_create_directory();
    if (!p->page_directory) return -1;

    // Load ELF into the process's own address space
    uint32_t entry = load_elf_into_dir(p->page_directory, elf_buf, elf_size);
    if (!entry) return -1;

    // Allocate and map user stack at USER_STACK_TOP
    // Virtual: [USER_STACK_TOP - N*PAGE_SIZE, USER_STACK_TOP)
    for (int i = 0; i < USER_STACK_PAGES; i++)
    {
        uint32_t va   = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        uint32_t phys = paging_alloc_page();
        if (!phys) return -1;
        paging_map(p->page_directory, va, phys,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    uint32_t user_sp = USER_STACK_TOP - 4;  // start just below top
    p->user_stack = user_sp;

    // Allocate a kernel stack for this process (used during syscalls/IRQs)
    p->kernel_stack = (uint8_t*)kmalloc(KERNEL_STACK_SIZE);
    if (!p->kernel_stack) return -1;
    uint32_t kstack_top = (uint32_t)(p->kernel_stack + KERNEL_STACK_SIZE);

    // Set up the kernel stack to look like we were interrupted at ring3.
    // context_switch will pop registers and ret into process_iret_trampoline,
    // which does iret using the ring3 frame we push here.
    uint32_t* ksp = (uint32_t*)kstack_top;

    // Ring-3 iret frame (consumed by process_iret_trampoline):
    *(--ksp) = 0x23;           // ss  (ring3 data segment)
    *(--ksp) = user_sp;        // esp (user stack)
    *(--ksp) = 0x00000202;     // eflags (IF=1)
    *(--ksp) = 0x1B;           // cs  (ring3 code segment)
    *(--ksp) = entry;          // eip (process entry point)

    // Fake return address — context_switch does ret which lands here
    *(--ksp) = (uint32_t)process_iret_trampoline;

    // Saved registers (popped by context_switch before the ret)
    *(--ksp) = 0;  // ebp
    *(--ksp) = 0;  // edi
    *(--ksp) = 0;  // esi
    *(--ksp) = 0;  // ebx

    p->regs.esp = (uint32_t)ksp;
    p->regs.eip = entry;

    process_count++;
    return slot;
}

// Legacy: create a process that runs a kernel function (used during boot).
int process_create(const char* name, void (*entry)())
{
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++)
        if (processes[i].state == PROCESS_DEAD) { slot = i; break; }
    if (slot == -1) return -1;

    process_t* p = &processes[slot];
    p->pid   = slot;
    p->state = PROCESS_READY;
    copy_str(p->name, name, 32);

    p->page_directory = paging_create_directory();
    p->user_stack     = 0;

    p->kernel_stack = (uint8_t*)kmalloc(KERNEL_STACK_SIZE);
    if (!p->kernel_stack) return -1;
    uint32_t kstack_top = (uint32_t)(p->kernel_stack + KERNEL_STACK_SIZE);

    uint32_t* ksp = (uint32_t*)kstack_top;
    *(--ksp) = 0x00000202;
    *(--ksp) = 0x08;
    *(--ksp) = (uint32_t)entry;
    *(--ksp) = (uint32_t)process_iret_trampoline;
    *(--ksp) = 0;
    *(--ksp) = 0;
    *(--ksp) = 0;
    *(--ksp) = 0;

    p->regs.esp = (uint32_t)ksp;
    p->regs.eip = (uint32_t)entry;

    process_count++;
    return slot;
}

void process_exit()
{
    if (current_pid == 0) return;

    processes[current_pid].state = PROCESS_DEAD;
    process_count--;

    // Find next ready process
    int next = -1;
    for (int i = 1; i < MAX_PROCESSES; i++)
        if (processes[i].state == PROCESS_READY) { next = i; break; }

    if (next == -1)
    {
        // Back to kernel idle
        current_pid = 0;
        processes[0].state = PROCESS_RUNNING;
        paging_switch(kernel_directory);
        __asm__ volatile(
            "mov %0, %%esp\n"
            "sti\n"
            "1: hlt\n"
            "jmp 1b\n"
            :: "r"(processes[0].regs.esp)
        );
    }

    processes[next].state = PROCESS_RUNNING;
    int old = current_pid;
    current_pid = next;

    tss_set_kernel_stack((uint32_t)(processes[next].kernel_stack + KERNEL_STACK_SIZE));
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
        if (current_pid != 0 && processes[0].state != PROCESS_DEAD)
            next = 0;
        else
            return;
    }

    processes[current_pid].state = PROCESS_READY;
    processes[next].state        = PROCESS_RUNNING;

    int old = current_pid;
    current_pid = next;

    // Switch TSS kernel stack so syscalls from the new process use the right stack
    tss_set_kernel_stack((uint32_t)(processes[next].kernel_stack + KERNEL_STACK_SIZE));
    paging_switch(processes[next].page_directory);
    context_switch(&processes[old].regs.esp, &processes[next].regs.esp);
}

// ── exec: replace current process with a new ELF from disk ───────────────────
// Loads the ELF at `path` into the current process's address space.
// Clears all existing user mappings, loads new segments, resets stack.
// Does not return on success — jumps directly to the new entry point.
// Returns -1 on failure (file not found, bad ELF, etc.)
int sys_exec(const char* path)
{
    // Read the ELF file from VFS
    extern int   vfs_open(const char*, int);
    extern int   vfs_read(int, uint8_t*, uint32_t);
    extern int   vfs_close(int);
    extern int   vfs_stat(const char*, uint32_t*);

    uint32_t file_size = 0;
    if (vfs_stat(path, &file_size) < 0) return -1;
    if (file_size == 0 || file_size > 4*1024*1024) return -1;  // max 4MB

    // Allocate buffer for ELF (in kernel heap)
    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) return -1;

    int fd = vfs_open(path, 1);  // VFS_O_READ
    if (fd < 0) { kfree(buf); return -1; }

    uint32_t total = 0;
    int bytes;
    while (total < file_size) {
        bytes = vfs_read(fd, buf + total, file_size - total);
        if (bytes <= 0) break;
        total += bytes;
    }
    vfs_close(fd);

    if (total < 52) { kfree(buf); return -1; }

    // Validate ELF
    if (*(uint32_t*)buf != 0x464C457F) { kfree(buf); return -1; }

    process_t* p = process_current();

    // Create a fresh page directory for the new image
    uint32_t* new_dir = paging_create_directory();
    if (!new_dir) { kfree(buf); return -1; }

    // Load ELF into new directory
    uint32_t entry = load_elf_into_dir(new_dir, buf, total);
    kfree(buf);
    if (!entry) return -1;

    // Map user stack in new directory
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t va   = USER_STACK_TOP - (i+1) * PAGE_SIZE;
        uint32_t phys = paging_alloc_page();
        if (!phys) return -1;
        paging_map(new_dir, va, phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    // Switch to new address space
    p->page_directory = new_dir;
    p->user_stack     = USER_STACK_TOP - 4;
    paging_switch(new_dir);

    // Update TSS kernel stack
    extern void tss_set_kernel_stack(uint32_t);
    tss_set_kernel_stack((uint32_t)(p->kernel_stack + KERNEL_STACK_SIZE));

    // Jump to new entry point at ring 3
    extern void jump_to_ring3(void (*entry)(), uint32_t user_stack);
    jump_to_ring3((void*)entry, USER_STACK_TOP - 4);

    // Never reached
    return 0;
}

// ── spawn: launch a new process from an ELF on disk ──────────────────────────
// Creates a new process slot, loads the ELF, and marks it ready.
// The new process inherits the current TTY.
// Returns the new PID, or -1 on failure.
int sys_spawn(const char* path)
{
    extern int   vfs_open(const char*, int);
    extern int   vfs_read(int, uint8_t*, uint32_t);
    extern int   vfs_close(int);
    extern int   vfs_stat(const char*, uint32_t*);

    uint32_t file_size = 0;
    if (vfs_stat(path, &file_size) < 0) return -1;
    if (file_size == 0 || file_size > 4*1024*1024) return -1;

    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) return -1;

    int fd = vfs_open(path, 1);
    if (fd < 0) { kfree(buf); return -1; }

    uint32_t total = 0;
    int bytes;
    while (total < file_size) {
        bytes = vfs_read(fd, buf + total, file_size - total);
        if (bytes <= 0) break;
        total += bytes;
    }
    vfs_close(fd);

    if (total < 52) { kfree(buf); return -1; }
    if (*(uint32_t*)buf != 0x464C457F) { kfree(buf); return -1; }

    int pid = process_create_elf("program", buf, total);
    kfree(buf);
    if (pid < 0) return -1;

    // Inherit TTY from parent
    extern int tty_for_pid[];
    extern int fbterm_pid_tty[];
    int parent_tty = tty_for_pid[process_current()->pid];
    tty_for_pid[pid]    = parent_tty;
    fbterm_pid_tty[pid] = parent_tty;

    return pid;
}

// ── wait: block until a process exits ────────────────────────────────────────
int process_is_alive(int pid)
{
    if (pid < 0 || pid >= MAX_PROCESSES) return 0;
    return processes[pid].state != PROCESS_DEAD;
}

void sys_wait(int pid)
{
    if (pid < 0 || pid >= MAX_PROCESSES) return;
    while (process_is_alive(pid))
        scheduler();
}
