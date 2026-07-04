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
#include "../include/userproc64.h"
#include "../include/physmem64.h"
#include "../include/console64.h"

// Comment out to skip the deliberate int3/ud2 exception tests — the
// PIC/IRQ/sti bring-up below always runs regardless of this flag.
// #define ISR64_RUN_TESTS 1

// Define to run the Milestone 3B hardcoded ring3 smoke test in place of
// the normal interactive boot below -- the two are mutually exclusive
// (the ring3 test halts forever once it catches the deliberate #UD, so
// there is no "resume the scheduler afterward").
// #define RING3_TEST64_RUN 1

// Define to run the Milestone 7 user process lifecycle test in place of
// the normal interactive boot -- loads a real compiled program from
// TxFS64, tracks it as a real process, runs it in ring3, and returns
// control to the kernel scheduler once it exits/faults. All debug modes
// below are mutually exclusive with each other and with the default.
// Milestone 16: the exec64_test*/exec64_fault_test/etc. fixtures this
// loads are no longer on the disk image by default -- rebuild it with
// `make populate PACKAGE_DEBUG64=1` before re-enabling this flag, or
// userproc64_run below will fail to find the binary.
// #define EXEC64_TEST_RUN 1

// Define to stop right after the boot diagnostics below and fall into
// the Milestone 3A kernel-task scheduler WITHOUT ever launching
// userland -- this was the default before Milestone 13. Useful for
// debugging boot/IDT/TSS/ATA/TxFS bring-up by itself, without a
// process/shell on top, while keeping the diagnostics on screen.
// #define KERNEL64_DIAG_ONLY 1

// Milestone 13: the NORMAL boot path (none of the debug flags above
// defined) launches /init64.nex64, which launches /shell64.nex64 by
// default (see user64/init64.c's own INIT64_TEST_MODE flag for its
// debug alternative) -- instead of leaving the user stuck on the boot
// diagnostics screen with no prompt. The diagnostics below still run
// and still go to klog/serial either way; only the VGA screen gets
// cleared (vgaterm64_clear, right before the handoff) so the user lands
// on a clean shell prompt instead of a wall of boot text.

extern void timer64_handler(void);
extern void keyboard64_handler(void);

_Static_assert(sizeof(void*) == 8, "kernel64.c must be compiled as 64-bit (-m64)");

static int my_kstrlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

// Writes one line to both the console (FB or VGA) and the serial/klog
// ring buffer.  Replaces the old vga_puts()-based path: output now goes
// through console64 so it appears on whichever backend is active.
static void out_line(const char* s) {
    console64_write(s, (uint64_t)my_kstrlen(s));
    console64_write("\n", 1);
    klog(s);
    klog("\n");
}

// Parse the multiboot2 info struct for a framebuffer tag (type 8).
// The mb_info_addr block is in low physical memory, which is identity-
// mapped (VA == PA) so the pointer is valid without any translation.
// Returns without modifying the out-parameters if no framebuffer tag is
// found or if the reported framebuffer type is not 2 (direct RGB color).
static void mb2_find_fb(uint64_t mb_info_addr,
    uint64_t* fb_addr, uint32_t* fb_width, uint32_t* fb_height,
    uint32_t* fb_pitch, uint8_t* fb_bpp)
{
    *fb_addr = 0;
    if (!mb_info_addr) return;

    uint32_t total = *(uint32_t*)(uintptr_t)mb_info_addr;
    uint8_t* p   = (uint8_t*)(uintptr_t)(mb_info_addr + 8);
    uint8_t* end = (uint8_t*)(uintptr_t)(mb_info_addr + (uint64_t)total);

    while (p + 8 <= end) {
        uint32_t type = *(uint32_t*)p;
        uint32_t size = *(uint32_t*)(p + 4);
        if (type == 0) break; // end tag

        if (type == 8) {
            // Accept type 1 (RGB) or type 0 (indexed — QEMU's Bochs VGA
            // reports type 1 for 32bpp direct-color mode). 32bpp only.
            if (size >= 31 && *(uint8_t*)(p + 28) == 32 &&
                (*(uint8_t*)(p + 29) == 2 || *(uint8_t*)(p + 29) == 1)) {
                *fb_addr   = *(uint64_t*)(p + 8);
                *fb_pitch  = *(uint32_t*)(p + 16);
                *fb_width  = *(uint32_t*)(p + 20);
                *fb_height = *(uint32_t*)(p + 24);
                *fb_bpp    = 32;
            }
            break;
        }

        // Advance to next tag (each tag is 8-byte aligned)
        uint32_t skip = (size + 7u) & ~7u;
        if (!skip) break;
        p += skip;
    }
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
    char line[96];
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
    // Milestone 21: parse framebuffer info from multiboot2 before any output
    // so that boot diagnostics appear on the framebuffer when available.
    uint64_t fb_addr = 0;
    uint32_t fb_width = 0, fb_height = 0, fb_pitch = 0;
    uint8_t  fb_bpp = 0;
    mb2_find_fb(mb_info_addr, &fb_addr, &fb_width, &fb_height, &fb_pitch, &fb_bpp);
    klog_hex("fb_addr:", (uint32_t)fb_addr);
    // physmem64_init MUST come before console64_init: the framebuffer
    // mapping allocates a PD page via physmem64_alloc_page, and physmem64_init
    // resets used_bitmap to 0 — if called after, it un-tracks that page and
    // subsequent allocs zero it, destroying the FB page table entries.
    physmem64_init();
    console64_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);

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

    out_line("physmem64 pool initialized");

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

#if defined(RING3_TEST64_RUN)
    out_line("Entering ring3 syscall test (iretq) -- expect sys64_write/sys64_exit next");
    ring3_test64_start();
    // unreachable: ring3_enter64 ends in iretq, and the stub's sys64_exit
    // (Milestone 5) halts forever once it's done.
#elif defined(EXEC64_TEST_RUN)
    // Path is a literal here on purpose, swapped by hand between
    // /exec64_test.nex64 (primary), /exec64_test.elf64 (fallback proof),
    // /exec64_fault_test.nex64 (pid-aware fault termination proof),
    // /exec64_isolation_test.nex64 (per-process memory isolation proof),
    // /exec64_badptr_test.nex64 (user-pointer validation proof), and
    // /hello.nex (wrong-arch rejection proof) during verification -- see
    // the Milestone 8/9 plans' test matrices. Run TWICE in a row: proves
    // two distinct, incrementing real pids, and that the second run's
    // address space is freshly created/torn down rather than erroring on
    // stale state left over from the first.
    const char* exec64_test_path = "/exec64_test.nex64";
    for (int run = 1; run <= 2; run++) {
        out_kv("userproc64: run #", (uint64_t)run);
        klog(exec64_test_path);
        klog("\n");
        int code = userproc64_run(exec64_test_path, 0, 0);
        out_kv("userproc64: returned to kernel, exit code: ", (uint64_t)(int64_t)code);
    }
    process64_init();
    process64_start();
    // Reached only if the scheduler later switches back to the boot
    // task -- proves control genuinely returned to the kernel (it
    // doesn't matter whether userproc64_run succeeded or failed; either
    // way execution falls through here).
#elif defined(KERNEL64_DIAG_ONLY)
    process64_init();
    process64_start();
    // Reached only if the scheduler later switches back to the boot
    // task (e.g. if both demo tasks ever died) — falls into the same
    // idle loop as before. Diagnostics stay on screen -- no userland,
    // no VGA clear.
#else
    // Milestone 13: normal interactive boot. One last line on the
    // diagnostics screen, then clear it before init64/shell64 ever get
    // a chance to print anything -- the user should land on a clean
    // shell prompt, not a wall of boot text (which is still fully
    // logged to klog/serial regardless).
    out_line("Launching ToxenOS64 interactive shell...");
    console64_clear();
    int init_code = userproc64_run("/init64.nex64", 0, 0);
    klog_hex("userproc64: init64 returned to kernel, exit code: ", (uint32_t)(int64_t)init_code);
    process64_init();
    process64_start();
    // Reached only if the scheduler later switches back to the boot
    // task -- proves control genuinely returned to the kernel.
#endif

    for (;;) {
        __asm__ volatile("hlt");
    }
}
