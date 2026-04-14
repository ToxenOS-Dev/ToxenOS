#include "../include/memmap.h"
#include "../include/process.h"
#include "../include/mm.h"
#include "../include/paging.h"
#include "../include/vga.h"
#include "../include/tss.h"
#include "../include/timer.h"
#include "../include/vfs.h"

process_t processes[MAX_PROCESSES];
static int       current_pid   = 0;
static int       process_count = 0;
static uint32_t  next_pid      = 1;  // monotonically increasing; never reuses a PID

extern void context_switch(uint32_t* old_esp, uint32_t* new_esp);
extern void process_iret_trampoline();
extern uint32_t stack_top;  // kernel stack top from boot.asm

static void copy_str(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max-1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ── Kernel stack allocator ────────────────────────────────────────────────────
// Each process gets KERNEL_STACK_SIZE bytes of kernel stack backed by real
// physical pages, with one unmapped guard page immediately below the stack.
// If the stack overflows it hits the guard page → clean kernel panic instead
// of silent heap corruption.
//
// Virtual layout for slot N (base = KSTACK_VIRT_BASE + N * KSTACK_SLOT_SIZE):
//
//   base + 0                        ← guard page (NOT mapped — catches overflow)
//   base + PAGE_SIZE                ← stack bottom (mapped)
//   base + PAGE_SIZE + KERNEL_STACK_SIZE - 1  ← stack top (highest used byte)
//
// kstack_alloc() returns the stack BASE pointer (caller adds KERNEL_STACK_SIZE
// to get the top, exactly like kmalloc did).

static uint8_t kstack_used[MAX_PROCESSES];  // 1 = slot in use

static uint8_t* kstack_alloc(void)
{
    for (int slot = 0; slot < MAX_PROCESSES; slot++) {
        if (kstack_used[slot]) continue;

        uint32_t base   = KSTACK_VIRT_BASE + (uint32_t)slot * KSTACK_SLOT_SIZE;
        uint32_t stack_base = base + PAGE_SIZE;   // guard page at base, stack above

        // Map KERNEL_STACK_SIZE / PAGE_SIZE pages for the actual stack
        uint32_t pages = KERNEL_STACK_SIZE / PAGE_SIZE;
        for (uint32_t i = 0; i < pages; i++) {
            uint32_t va   = stack_base + i * PAGE_SIZE;
            uint32_t phys = paging_alloc_page();
            if (!phys) {
                // Roll back already-mapped pages
                for (uint32_t j = 0; j < i; j++) {
                    uint32_t v2 = stack_base + j * PAGE_SIZE;
                    uint32_t p2 = kernel_directory[v2 >> 22];
                    if (p2 & PAGE_PRESENT) {
                        uint32_t* tbl = (uint32_t*)KPHYS_TO_VIRT(p2 & ~0xFFF);
                        uint32_t entry = tbl[(v2 >> 12) & 0x3FF];
                        if (entry & PAGE_PRESENT)
                            paging_free_page(entry & ~0xFFF);
                    }
                    paging_unmap(kernel_directory, v2);
                }
                return 0;
            }
            // Kernel stack pages: present + writable, NO PAGE_USER
            paging_map(kernel_directory, va, phys, PAGE_PRESENT | PAGE_WRITABLE);
        }
        // Guard page: leave base unmapped (already zero in directory)

        kstack_used[slot] = 1;
        return (uint8_t*)stack_base;
    }
    return 0;  // no free slots
}

static void kstack_free(uint8_t* stack_base)
{
    if (!stack_base) return;

    uint32_t base = (uint32_t)stack_base - PAGE_SIZE;  // guard page is one page below
    int slot = (int)((base - KSTACK_VIRT_BASE) / KSTACK_SLOT_SIZE);
    if (slot < 0 || slot >= MAX_PROCESSES) return;
    if (!kstack_used[slot]) return;

    // Unmap and free each stack page
    uint32_t pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t va = (uint32_t)stack_base + i * PAGE_SIZE;
        // Get physical address from the page table
        uint32_t dir_entry = kernel_directory[va >> 22];
        if (dir_entry & PAGE_PRESENT) {
            uint32_t* tbl = (uint32_t*)KPHYS_TO_VIRT(dir_entry & ~0xFFF);
            uint32_t  pte = tbl[(va >> 12) & 0x3FF];
            if (pte & PAGE_PRESENT)
                paging_free_page(pte & ~0xFFF);
        }
        paging_unmap(kernel_directory, va);
    }
    kstack_used[slot] = 0;
}

void process_init()
{
    // Zero the entire array so no garbage fields cause issues
    uint8_t* base = (uint8_t*)processes;
    for (uint32_t i = 0; i < sizeof(processes); i++) base[i] = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].state       = PROCESS_DEAD;
        processes[i].waiting_for = -1;
        processes[i].stdin_fd    = -1;
        processes[i].stdout_fd   = -1;
        processes[i].tty         = -1;
    }

    processes[0].pid            = 0;
    processes[0].state          = PROCESS_RUNNING;
    processes[0].waiting_for    = -1;
    processes[0].tty            = 0;
    processes[0].stdin_fd       = -1;
    processes[0].stdout_fd      = -1;
    processes[0].heap_end       = 0;
    fd_table_init(&processes[0].fds);
    processes[0].page_directory = kernel_directory;
    processes[0].user_stack     = 0;
    // kernel_stack for pid 0 points to the boot stack so that when the
    // scheduler updates TSS.esp0 for this process it uses the right address.
    // tss_set_kernel_stack(p->kernel_stack + KERNEL_STACK_SIZE) must equal &stack_top.
    extern uint32_t stack_top;
    processes[0].kernel_stack   = (uint8_t*)((uint32_t)&stack_top - KERNEL_STACK_SIZE);
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
    // ── ELF header validation ─────────────────────────────────────────────────
    if (elf_size < 52) return 0;
    if (*(uint32_t*)elf_buf != 0x464C457Fu) return 0;  // magic
    if (elf_buf[4] != 1) return 0;                      // ELF32
    if (elf_buf[5] != 1) return 0;                      // little-endian
    if (*(uint16_t*)(elf_buf + 16) != 2) return 0;      // ET_EXEC

    uint32_t entry     = *(uint32_t*)(elf_buf + 24);
    uint32_t phoff     = *(uint32_t*)(elf_buf + 28);
    uint16_t phentsize = *(uint16_t*)(elf_buf + 42);
    uint16_t phnum     = *(uint16_t*)(elf_buf + 44);

    // Program header table must be within the file
    if (phentsize < 32) return 0;
    if (phoff > elf_size) return 0;
    if ((uint32_t)phnum * phentsize > elf_size - phoff) return 0;

    // Entry point must be in user space
    if (entry < USER_ELF_BASE || entry >= USER_STACK_TOP) return 0;

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

        // ── Segment security checks ───────────────────────────────────────────
        // 1. Segment must be in user address space
        if (vaddr + memsz <= USER_ELF_BASE) continue;
        if (vaddr >= USER_STACK_TOP) return 0;

        // 2. memsz overflow check: vaddr + memsz must not wrap or enter kernel
        if (memsz > USER_STACK_TOP - vaddr) return 0;

        // 3. filesz must not exceed memsz
        if (filesz > memsz) return 0;

        // 4. file data must be within the ELF buffer
        if (offset > elf_size) return 0;
        if (filesz > elf_size - offset) return 0;

        // 5. total mapped size must fit within allowed ELF region
        if (vaddr + memsz > USER_ELF_BASE + USER_ELF_MAX_SIZE) return 0;

        uint32_t pf = PAGE_PRESENT | PAGE_USER;
        if (flags & 2) pf |= PAGE_WRITABLE;  // PF_W

        // page_start/page_end are safe after the overflow checks above
        uint32_t page_start = vaddr & ~0xFFFu;
        uint32_t page_end   = (vaddr + memsz + 0xFFFu) & ~0xFFFu;

        for (uint32_t va = page_start; va < page_end; va += PAGE_SIZE)
        {
            uint32_t phys = paging_alloc_page();
            if (!phys) return 0;

            paging_map(dir, va, phys, pf);

            uint8_t* dst = (uint8_t*)KPHYS_TO_VIRT(phys);

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

    // Free old kernel stack if reusing a dead slot
    if (p->kernel_stack) {
        kstack_free(p->kernel_stack);
        p->kernel_stack = 0;
    }

    p->pid          = next_pid++;
    p->state        = PROCESS_READY;
    p->waiting_for  = -1;
    p->tty          = -1;
    p->stdin_fd     = -1;
    p->stdout_fd    = -1;
    p->heap_end     = 0;
    fd_table_init(&p->fds);
    p->args[0]      = 0;
    copy_str(p->name, name, 32);

    // Allocate kernel stack FIRST — kstack_alloc may create new page table
    // entries in kernel_directory. paging_create_directory() copies those
    // entries, so it must run after kstack_alloc to get them.
    p->kernel_stack = kstack_alloc();
    if (!p->kernel_stack) return -1;

    // Create an isolated page directory for this process
    p->page_directory = paging_create_directory();
    if (!p->page_directory) { kstack_free(p->kernel_stack); p->kernel_stack = 0; return -1; }

    // Load ELF into the process's own address space
    uint32_t entry = load_elf_into_dir(p->page_directory, elf_buf, elf_size);
    if (!entry) { kstack_free(p->kernel_stack); p->kernel_stack = 0; return -1; }

    // Allocate and map user stack at USER_STACK_TOP
    for (int i = 0; i < USER_STACK_PAGES; i++)
    {
        uint32_t va   = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        uint32_t phys = paging_alloc_page();
        if (!phys) { kstack_free(p->kernel_stack); p->kernel_stack = 0; return -1; }
        paging_map(p->page_directory, va, phys,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    // Guard page below the user stack
    uint32_t guard_va = USER_STACK_TOP - (USER_STACK_PAGES + 1) * PAGE_SIZE;
    paging_unmap(p->page_directory, guard_va);

    uint32_t user_sp = USER_STACK_TOP - 4;
    p->user_stack = user_sp;

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
    return (int)p->pid;  // return PID, not slot index
}

// Legacy: create a process that runs a kernel function (used during boot).
int process_create(const char* name, void (*entry)())
{
    int slot = -1;
    for (int i = 1; i < MAX_PROCESSES; i++)
        if (processes[i].state == PROCESS_DEAD) { slot = i; break; }
    if (slot == -1) return -1;

    process_t* p = &processes[slot];

    // Free old kernel stack if reusing a dead slot
    if (p->kernel_stack) {
        kstack_free(p->kernel_stack);
        p->kernel_stack = 0;
    }

    p->pid         = next_pid++;
    p->state       = PROCESS_READY;
    p->waiting_for = -1;
    p->tty         = -1;
    p->stdin_fd    = -1;
    p->stdout_fd   = -1;
    p->args[0]     = 0;
    copy_str(p->name, name, 32);

    // kstack_alloc first — may create new kernel_directory entries
    p->kernel_stack = kstack_alloc();
    if (!p->kernel_stack) return -1;

    p->page_directory = paging_create_directory();
    p->user_stack     = 0;
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
    return (int)p->pid;  // return PID, not slot index
}

void process_exit()
{
    if (current_pid == 0) return;

    process_t* p = &processes[current_pid];
    p->state = PROCESS_DEAD;
    process_count--;

    // Close all open file descriptors
    fd_table_close_all(&p->fds);

    // Free all user-space pages (non-kernel page tables).
    // We walk the page directory and free any table/pages that were
    // allocated specifically for this process (not shared kernel tables).
    if (p->page_directory && p->page_directory != kernel_directory)
    {
        for (int i = 0; i < 1024; i++)
        {
            if (!(p->page_directory[i] & PAGE_PRESENT)) continue;
            uint32_t table_phys = p->page_directory[i] & ~0xFFF;

            if ((kernel_directory[i] & ~0xFFF) == table_phys) continue;

            // table_phys is a physical address — access via kernel virtual
            uint32_t* table = (uint32_t*)KPHYS_TO_VIRT(table_phys);
            for (int j = 0; j < 1024; j++)
            {
                if (table[j] & PAGE_PRESENT)
                    paging_free_page(table[j] & ~0xFFF);
            }
            paging_free_aligned(table);
        }
        paging_free_aligned(p->page_directory);
        p->page_directory = 0;
    }

    // Wake any process waiting on us
    for (int i = 0; i < MAX_PROCESSES; i++)
    {
        if (processes[i].state == PROCESS_WAITING &&
            processes[i].waiting_for == (int)p->pid)
        {
            processes[i].state       = PROCESS_READY;
            processes[i].waiting_for = -1;
        }
    }

    scheduler();
}

process_t* process_current()
{
    return &processes[current_pid];
}

process_t* process_get_by_pid(int pid)
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].pid == (uint32_t)pid && processes[i].state != PROCESS_DEAD)
            return &processes[i];
    return 0;
}

void scheduler()
{
    // Wake any sleeping processes whose timer has expired
    uint32_t now = timer_getticks();
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_SLEEPING &&
            now >= processes[i].sleep_until)
            processes[i].state = PROCESS_READY;
    }

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
        if (current_pid != 0 &&
            processes[0].state != PROCESS_DEAD &&
            processes[0].state != PROCESS_WAITING &&
            processes[0].state != PROCESS_SLEEPING)
            next = 0;
        else
            return;
    }

    // Preserve WAITING/SLEEPING/DEAD; only demote RUNNING → READY
    if (processes[current_pid].state == PROCESS_RUNNING)
        processes[current_pid].state = PROCESS_READY;

    processes[next].state = PROCESS_RUNNING;

    int old = current_pid;
    current_pid = next;

    tss_set_kernel_stack((uint32_t)(processes[next].kernel_stack + KERNEL_STACK_SIZE));
    paging_switch(processes[next].page_directory);
    context_switch(&processes[old].regs.esp, &processes[next].regs.esp);
}

// ── load_elf_from_path ────────────────────────────────────────────────────────
// Read an ELF file from the VFS into a kernel heap buffer.
// On success: *buf_out points to a kmalloc'd buffer the caller must kfree,
//             *size_out is the number of bytes read, returns 0.
// On failure: *buf_out is NULL, returns -1.
static int load_elf_from_path(const char* path, uint8_t** buf_out, uint32_t* size_out)
{
    extern int vfs_stat(const char*, uint32_t*);
    extern int vfs_open(const char*, int);
    extern int vfs_read(int, uint8_t*, uint32_t);
    extern int vfs_close(int);

    *buf_out  = 0;
    *size_out = 0;

    uint32_t file_size = 0;
    if (vfs_stat(path, &file_size) < 0) return -1;
    if (file_size == 0 || file_size > USER_ELF_MAX_SIZE) return -1;

    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) return -1;

    int fd = vfs_open(path, 1);
    if (fd < 0) { kfree(buf); return -1; }

    uint32_t total = 0;
    int n;
    while (total < file_size) {
        n = vfs_read(fd, buf + total, file_size - total);
        if (n <= 0) break;
        total += (uint32_t)n;
    }
    vfs_close(fd);

    if (total < 52 || *(uint32_t*)buf != 0x464C457F) {
        kfree(buf);
        return -1;
    }

    *buf_out  = buf;
    *size_out = total;
    return 0;
}

// ── exec: replace current process with a new ELF from disk ───────────────────
int sys_exec(const char* path)
{
    uint8_t* buf; uint32_t size;
    if (load_elf_from_path(path, &buf, &size) < 0) return -1;

    process_t* p = process_current();

    uint32_t* new_dir = paging_create_directory();
    if (!new_dir) { kfree(buf); return -1; }

    uint32_t entry = load_elf_into_dir(new_dir, buf, size);
    kfree(buf);
    if (!entry) return -1;

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t va   = USER_STACK_TOP - (i+1) * PAGE_SIZE;
        uint32_t phys = paging_alloc_page();
        if (!phys) return -1;
        paging_map(new_dir, va, phys, PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

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
int sys_spawn(const char* path)
{
    uint8_t* buf; uint32_t size;
    if (load_elf_from_path(path, &buf, &size) < 0) return -1;

    int pid = process_create_elf("program", buf, size);
    kfree(buf);
    if (pid < 0) return -1;

    int parent_tty = process_current()->tty;
    process_t* child = process_get_by_pid(pid);
    if (child) child->tty = parent_tty;

    return pid;
}

// ── wait: block until a process exits ────────────────────────────────────────
int process_is_alive(int pid)
{
    return process_get_by_pid(pid) != 0;
}

void sys_wait(int pid)
{
    if (!process_is_alive(pid)) return;

    process_t* me = process_current();
    me->state       = PROCESS_WAITING;
    me->waiting_for = pid;
    scheduler();
}

// Sleep for `ms` milliseconds. Timer runs at 100Hz so 1 tick = 10ms.
void sys_sleep(uint32_t ms)
{
    if (ms == 0) return;
    uint32_t ticks = (ms + 9) / 10;  // round up to nearest tick
    process_t* me = process_current();
    me->sleep_until = timer_getticks() + ticks;
    me->state       = PROCESS_SLEEPING;
    scheduler();
}

// Grow (or query) the process heap.
// increment > 0: map more bytes, return old heap_end (like POSIX sbrk)
// increment == 0: return current heap_end
// Returns (uint32_t)-1 on failure.
// USER_HEAP_BASE comes from memmap.h

// Maximum heap size: must not reach the user stack guard page
#define USER_HEAP_MAX  (USER_STACK_TOP - (USER_STACK_PAGES + 2) * PAGE_SIZE)

uint32_t sys_sbrk(int32_t increment)
{
    process_t* p = process_current();

    if (p->heap_end == 0)
        p->heap_end = USER_HEAP_BASE;

    if (increment == 0)
        return p->heap_end;

    if (increment < 0) {
        uint32_t new_end = p->heap_end + (uint32_t)increment;
        if (new_end < USER_HEAP_BASE) return (uint32_t)-1;
        p->heap_end = new_end;
        return p->heap_end;
    }

    uint32_t old_end  = p->heap_end;
    uint32_t new_end  = old_end + (uint32_t)increment;

    // Overflow check: new_end must not wrap and must stay below the stack
    if (new_end < old_end)         return (uint32_t)-1;  // integer overflow
    if (new_end > USER_HEAP_MAX)   return (uint32_t)-1;  // would hit stack

    // Map any new pages needed between old and new end
    uint32_t page_start = (old_end  + 0xFFFu) & ~0xFFFu;
    uint32_t page_end   = (new_end  + 0xFFFu) & ~0xFFFu;

    for (uint32_t va = page_start; va < page_end; va += PAGE_SIZE) {
        uint32_t phys = paging_alloc_page();
        if (!phys) {
            // OOM — roll back pages already mapped in this call
            for (uint32_t v2 = page_start; v2 < va; v2 += PAGE_SIZE)
                paging_unmap(p->page_directory, v2);
            return (uint32_t)-1;
        }
        paging_map(p->page_directory, va, phys,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    p->heap_end = new_end;
    return old_end;
}

// spawn on a specific TTY
int sys_spawn_tty(const char* path, int tty)
{
    uint8_t* buf; uint32_t size;
    if (load_elf_from_path(path, &buf, &size) < 0) return -1;

    int pid = process_create_elf("shell", buf, size);
    kfree(buf);
    if (pid < 0) return -1;

    process_t* child = process_get_by_pid(pid);
    if (child) child->tty = tty;

    return pid;
}

// spawn on a specific TTY with argument string
int sys_spawn_tty_args(const char* path, int tty, const char* args)
{
    int pid = sys_spawn_tty(path, tty);
    if (pid < 0) return -1;

    process_t* p = process_get_by_pid(pid);
    if (!p) return -1;

    if (args) {
        int i = 0;
        while (args[i] && i < 255) { p->args[i] = args[i]; i++; }
        p->args[i] = 0;
    } else {
        p->args[0] = 0;
    }
    return pid;
}

// exec_cmd: spawn external command on same TTY, mark it to respawn shell on exit
int sys_exec_cmd(const char* path, int tty, const char* args)
{
    int pid = sys_spawn_tty_args(path, tty, args);
    if (pid < 0) return -1;
    return pid;
}
