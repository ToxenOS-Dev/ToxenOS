// kernel/kernel64.c — ToxenOS x86_64 long-mode boot bring-up.
//
// Milestone 1: proves the boot64.asm long-mode transition genuinely
// succeeded (VGA + serial output, CR0/CR4/EFER readback including LMA).
// Milestone 2 (this revision): brings up the 64-bit IDT, optionally
// exercises int3/ud2 exception handling, then enables PIC/IRQ0/IRQ1 and
// `sti` before parking in the idle loop. Does not call into any existing
// 32-bit subsystem (cpu.c/acpi.c/paging.c/fbterm.c/...) — those get
// ported in their own later migration milestones, not here.
#include <stdint.h>
#include "../include/klog.h"
#include "../include/idt64.h"
#include "../include/irq64.h"
#include "../include/pic.h"
#include "../include/process64.h"
#include "../include/tss64.h"
#include "../include/ring3_test64.h"
#include "../include/ata64.h"
#include "../include/txfs64.h"
#include "../include/exec64.h"

// Comment out to skip the deliberate int3/ud2 exception tests — the
// PIC/IRQ/sti bring-up below always runs regardless of this flag.
// #define ISR64_RUN_TESTS 1

// Define to run the Milestone 3B hardcoded ring3 smoke test in place of
// the Milestone 3A kernel-task scheduler -- the two are mutually
// exclusive (the ring3 test halts forever once it catches the
// deliberate #UD, so there is no "resume the scheduler afterward").
// #define RING3_TEST64_RUN 1

// Define to run the Milestone 6 NEX64/ELF64 exec test in place of both
// of the above -- loads a real compiled program from TxFS64 and runs
// it in ring3. All three test modes are mutually exclusive.
// #define EXEC64_TEST_RUN 1

extern void timer64_handler(void);
extern void keyboard64_handler(void);

_Static_assert(sizeof(void*) == 8, "kernel64.c must be compiled as 64-bit (-m64)");

static uint16_t* const VGA = (uint16_t*)0xB8000;
#define VGA_COLS 80
#define VGA_ROWS 25

static int vga_row = 0;

static void vga_puts(const char* s, uint8_t color) {
    if (vga_row >= VGA_ROWS) return;
    int col = 0;
    for (int i = 0; s[i] && col < VGA_COLS; i++) {
        VGA[vga_row * VGA_COLS + col] = ((uint16_t)color << 8) | (uint8_t)s[i];
        col++;
    }
    vga_row++;
}

// Writes one line to both VGA text memory and the serial/klog ring buffer.
static void out_line(const char* s) {
    vga_puts(s, 0x0F);
    klog(s);
    klog("\n");
}

static void hex64(uint64_t val, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) {
        out[2 + (15 - i)] = h[val & 0xF];
        val >>= 4;
    }
    out[18] = 0;
}

static void out_kv(const char* label, uint64_t val) {
    char line[48];
    int i = 0;
    while (label[i]) { line[i] = label[i]; i++; }
    char hex[19];
    hex64(val, hex);
    int j = 0;
    while (hex[j]) { line[i++] = hex[j++]; }
    line[i] = 0;
    out_line(line);
}

static void append_flag(char* buf, int* pos, const char* name, int set) {
    int i = 0;
    while (name[i]) buf[(*pos)++] = name[i++];
    buf[(*pos)++] = '=';
    buf[(*pos)++] = set ? '1' : '0';
    buf[(*pos)++] = ' ';
}

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_efer(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    return ((uint64_t)hi << 32) | lo;
}

void kernel_main64(uint64_t magic, uint64_t mb_info_addr) {
    vga_row = 0;

    out_line("ToxenOS64 -- Milestone 1: long-mode boot");
    out_kv("multiboot magic: ", magic);
    out_kv("mb_info_addr:    ", mb_info_addr);

    uint64_t cr0  = read_cr0();
    uint64_t cr4  = read_cr4();
    uint64_t efer = read_efer();
    out_kv("CR0:  ", cr0);
    out_kv("CR4:  ", cr4);
    out_kv("EFER: ", efer);

    // PG (CR0 bit31), PAE (CR4 bit5), LME (EFER bit8), LMA (EFER bit10).
    // LMA in particular is set by the CPU only once the long-mode
    // transition has genuinely completed — distinct proof from LME,
    // which is just the software enable request.
    char flags[64];
    int pos = 0;
    append_flag(flags, &pos, "PG",  (int)((cr0  >> 31) & 1));
    append_flag(flags, &pos, "PAE", (int)((cr4  >> 5)  & 1));
    append_flag(flags, &pos, "LME", (int)((efer >> 8)  & 1));
    append_flag(flags, &pos, "LMA", (int)((efer >> 10) & 1));
    flags[pos] = 0;
    out_line(flags);

    out_line("TOXENOS64 LONGMODE OK");

    idt64_init();
    out_line("IDT64 initialized");

    tss64_init();
    out_line("TSS64 loaded (ltr)");

#ifdef ISR64_RUN_TESTS
    out_line("Triggering int3 (breakpoint) test...");
    __asm__ volatile ("int3");
    out_line("...returned from int3 OK");

    out_line("Triggering ud2 (invalid opcode) test...");
    __asm__ volatile ("ud2");
    // unreachable: #UD's saved RIP points at the faulting instruction
    // itself (no architectural "skip past it"), so the dispatcher halts
    // instead of returning here.
#endif

    pic_remap();
    irq64_register(0, timer64_handler);
    irq64_register(1, keyboard64_handler);
    out_line("PIC remapped, IRQ0/IRQ1 registered");

    __asm__ volatile ("sti");
    out_line("Interrupts enabled (sti) -- starting kernel tasks");

    ata64_init();
    out_line("ATA64 initialized");

    if (txfs64_mount() == 0) {
        out_line("TxFS64 mounted");
        int fd = txfs64_open("/hello.ts");
        if (fd >= 0) {
            uint8_t buf[512];
            int n = txfs64_read(fd, buf, sizeof(buf) - 1);
            buf[n > 0 ? (uint32_t)n : 0] = 0;
            out_kv("/hello.ts size: ", (uint64_t)n);
            klog((char*)buf);
            klog("\n");
            txfs64_close(fd);
        } else {
            out_line("/hello.ts: open failed");
        }
    } else {
        out_line("TxFS64 mount failed (bad magic)");
    }

#if defined(EXEC64_TEST_RUN)
    // Path is a literal here on purpose, swapped by hand between
    // /exec64_test.nex64 (primary), /exec64_test.elf64 (fallback proof),
    // and /hello.nex (wrong-arch rejection proof) during verification --
    // see the Milestone 6 plan's test matrix.
    const char* exec64_test_path = "/exec64_test.nex64";
    out_line("Loading via exec64...");
    klog(exec64_test_path);
    klog("\n");
    if (exec64_load_and_run(exec64_test_path) < 0)
        out_line("exec64: load failed -- see above");
    // unreachable on success: ring3_enter64 ends in iretq, and the
    // loaded program's own sys64_exit halts forever once it's done.
#elif defined(RING3_TEST64_RUN)
    out_line("Entering ring3 syscall test (iretq) -- expect sys64_write/sys64_exit next");
    ring3_test64_start();
    // unreachable: ring3_enter64 ends in iretq, and the stub's sys64_exit
    // (Milestone 5) halts forever once it's done.
#else
    process64_init();
    process64_start();
    // Reached only if the scheduler later switches back to the boot
    // task (e.g. if both demo tasks ever died) — falls into the same
    // idle loop as before.
#endif

    for (;;) {
        __asm__ volatile("hlt");
    }
}
