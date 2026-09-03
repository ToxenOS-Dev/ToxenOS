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
#include "../include/tss64.h"
#include "../include/ring3_test64.h"
#include "../include/ata64.h"
#include "../include/txfs64.h"
#include "../include/vfs64.h"
#include "../include/exec64.h"
#include "../include/process64.h"
#include "../include/uservm64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/lapic64.h"
#include "../include/console64.h"
#include "../include/pipe64.h"
#include "../include/shm64.h"
#include "../include/blockdev64.h"
#include "../include/pci64.h"
#include "../include/ahci64.h"
#include "../include/nvme64.h"
#include "../include/virtio_blk64.h"
#include "../include/pixfmt64.h"
#include "../include/display64.h"
#include "../include/ps2_64.h"
#include "../include/input64.h"
#include "../include/mouse64.h"
#include "../include/service64.h"
#include "../include/rustffi64.h"

// Comment out to skip the deliberate int3/ud2 exception tests — the
// PIC/IRQ/sti bring-up below always runs regardless of this flag.
// #define ISR64_RUN_TESTS 1

// Define to run the Milestone 29 pixel-format self-test suite -- pure
// logic, no hardware/allocator dependency, so it can run at any point;
// placed first since it needs nothing else initialized yet. Exercises
// pack()/from_mb2() against synthetic RGB/BGR/16bpp/24bpp layouts QEMU's
// fixed VBE emulation can never actually produce, so real boot tests
// can't cover them -- see include/pixfmt64.h.
// #define PIXFMT64_RUN_TESTS 1

// Define to run the Milestone 29 display64 self-test suite right after
// console64_init() -- exercises fill_rect/copy_rows/clipping against
// the real active framebuffer via direct pixel read-back, including a
// deliberately non-CHAR_H-aligned rectangle. Scribbles over the screen
// and leaves it cleared to black; harmless before the boot diagnostics
// below start writing (and they immediately paint over it either way).
// #define DISPLAY64_RUN_TESTS 1

// Define to run the Milestone 23 physical memory manager self-test
// suite right after physmem64_init(), before anything else ever calls
// physmem64_alloc_page/pages -- its exhaustion/reuse assertions require
// starting from a freshly initialized allocator. Leaves every region's
// free-page count restored to its pre-test value on a full pass.
// #define PHYSMEM64_RUN_TESTS 1

// Define to run the Milestone 22 kernel heap self-test suite right
// after heap64_init(), before anything else ever calls kmalloc/kfree --
// its span-count assertions require starting from an idle heap. Leaves
// the heap fully released (zero spans) on a full pass, so the rest of
// boot proceeds with an untouched physmem64 allocator either way.
// #define HEAP64_RUN_TESTS 1

// Define to run the Milestone 24 process/scheduler self-test suite
// after TxFS64 is mounted (it spawns real NEX64 test binaries from
// disk) and interrupts are enabled (it needs the timer to actually
// fire to prove preemption/blocking). Requires
// `make populate PACKAGE_DEBUG64=1` -- the sched_worker64 and
// exec_fault_test fixtures it spawns are not on the disk image by
// default. Runs before the normal boot path spawns init64, and fully
// reaps everything it creates on a pass.
// #define PROCESS64_RUN_TESTS 1

// Define to run the Milestone 25 brk/mmap self-test suite, same timing
// requirements as PROCESS64_RUN_TESTS (real spawns, real preemption) --
// requires `make populate PACKAGE_DEBUG64=1` for brk_mmap_test64.nex64.
// #define USERVM64_RUN_TESTS 1

// Define to run the Milestone 26 pipe self-test suite, same timing
// requirements as PROCESS64_RUN_TESTS -- requires
// `make populate PACKAGE_DEBUG64=1` for pipe_test64.nex64.
// #define PIPE64_RUN_TESTS 1

// Define to run the Milestone 26 shared-memory self-test suite, same
// timing requirements as PROCESS64_RUN_TESTS -- requires
// `make populate PACKAGE_DEBUG64=1` for shm_test64.nex64.
// #define SHM64_RUN_TESTS 1

// Define to run the Milestone 27 TxFS64 block/indirect-write self-test
// suite right after TxFS64 mounts, before init64/shell64 launch --
// operates directly on inode numbers, no process/VFS-handle machinery
// needed, so (unlike the other post-Milestone-24 suites) this one does
// NOT require PACKAGE_DEBUG64 or any extra fixture on the disk image.
// #define TXFS64_RUN_TESTS 1

// Define to run the Milestone 27 VFS-level self-test suite (path
// normalization, open/read/write/close through the unified handle
// table, directory enumeration, spawn inheritance, mixed handle kinds)
// -- same timing requirements as PROCESS64_RUN_TESTS -- requires
// `make populate PACKAGE_DEBUG64=1` for vfs_test64.nex64.
// #define VFS64_RUN_TESTS 1

// Define to run the Milestone 32 named-service registry self-test
// suite -- same timing requirements as PROCESS64_RUN_TESTS (real
// spawns, real preemption, real blocking) -- requires
// `make populate PACKAGE_DEBUG64=1` for service_test64.nex64.
// #define SERVICE64_RUN_TESTS 1

// Define to spawn /display_test64.nex64 during boot (needs
// `make populate PACKAGE_DEBUG64=1`) -- see user64/display_test64.c.
// #define DISPLAY_TEST64_RUN 1

// Define to spawn /input_test64.nex64 during boot -- it blocks on real
// keyboard/mouse events, drive it via QEMU monitor or interactively.
// Needs `make populate PACKAGE_DEBUG64=1` -- see user64/input_test64.c.
// #define INPUT_TEST64_RUN 1

// Define to run the Milestone 31 Rust/C integration self-test suite
// (rust/toxenos_rs/src/selftest.rs) right after heap64_init() -- it
// needs kmalloc/kfree and physmem64_alloc_page/free_page already
// working, same timing requirement as every allocator-touching C
// self-test in this file, but nothing else (no process/VFS layer
// involved). Logs each case and a pass/fail summary via klog().
// #define RUST64_SELFTEST_RUN 1

// Define to hand the first enumerated PCI device to the Milestone 31
// Rust PCI demo module (rust/toxenos_rs/src/pcidemo.rs) right after
// pci64_enumerate() -- read-only, never touches the device's registers
// or takes ownership of it. A no-op (logs nothing) if no PCI device was
// found, which never happens under QEMU (the host bridge is always
// device 0 on bus 0).
// #define RUST64_PCI_DEMO_RUN 1

// Define to log full PCI enumeration and block-device registry
// diagnostics (kernel/pci64.c's pci64_dump(), kernel/blockdev64.c's
// blockdev64_dump()) right after storage drivers initialize --
// verbose, development use only, never needed for normal boot.
// #define STORAGE64_DUMP 1

// Define to log framebuffer/display and input-subsystem diagnostics
// (kernel/display64.c's display64_dump(), kernel/mouse64.c's
// mouse64_dump(), kernel/input64.c's input64_dump()) right after
// display/input init -- verbose, development use only.
// #define DISPLAY_INPUT64_DUMP 1

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
// process64_spawn below will fail to find the binary.
// Milestone 24: exec64_test.nex64 now runs via process64_spawn +
// process64_wait (blocking, from kernel/idle context) instead of the
// old synchronous userproc64_run -- behaviorally identical from this
// file's point of view (still prints two distinct incrementing pids).
// #define EXEC64_TEST_RUN 1

// Define to stop right after the boot diagnostics below WITHOUT ever
// launching userland -- useful for debugging boot/IDT/TSS/ATA/TxFS
// bring-up by itself, without a process/shell on top, while keeping the
// diagnostics on screen. Milestone 24: the old Milestone 3A ring-0 task
// demo this used to fall into is gone (there is only one process model
// now); this just idles forever with zero processes instead.
// #define KERNEL64_DIAG_ONLY 1

// Define to deliberately trigger a Rust panic in place of the normal
// interactive boot -- verifies rust/toxenos_rs/src/panic.rs's handler
// (mutually exclusive with every other debug mode above/below: a panic
// halts forever, there's no "resume boot afterward"). See
// rust/toxenos_rs/src/selftest.rs's toxenos_rust_panic_test.
// #define RUST64_PANIC_TEST_RUN 1

// Define to deliberately request an impossible allocation in place of
// the normal interactive boot -- observes stable no_std Rust's actual
// out-of-memory behavior (see rust/toxenos_rs/src/selftest.rs's
// toxenos_rust_alloc_fail_test and the Milestone 31 summary's findings).
// Mutually exclusive with normal boot, same reasoning as
// RUST64_PANIC_TEST_RUN above.
// #define RUST64_ALLOC_FAIL_TEST_RUN 1

// Milestone 13: the NORMAL boot path (none of the debug flags above
// defined) launches /init64.nex64, which launches /shell64.nex64 by
// default (see user64/init64.c's own INIT64_TEST_MODE flag for its
// debug alternative) -- instead of leaving the user stuck on the boot
// diagnostics screen with no prompt. The diagnostics below still run
// and still go to klog/serial either way; only the VGA screen gets
// cleared (vgaterm64_clear, right before the handoff) so the user lands
// on a clean shell prompt instead of a wall of boot text.

extern void timer64_handler(void);
extern void timer64_init(uint32_t frequency);
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
//
// Milestone 29: no longer assumes every framebuffer is packed
// 0x00RRGGBB 32bpp -- reads the tag's actual color_info (red/green/blue
// field position + mask size for framebuffer_type==1, "direct RGB")
// and hands it to pixfmt64_from_mb2() for validation, so a genuinely
// unsupported layout (unexpected bpp, overlapping/out-of-range fields)
// is rejected cleanly here rather than silently mis-rendered later.
// framebuffer_type==0 (indexed) has no RGB field metadata to read at
// all; QEMU's default Bochs VBE mode is occasionally reported this way
// for what is actually its standard 32bpp XRGB8888 direct-color mode,
// so that one specific legacy case is still special-cased to the
// standard XRGB8888 layout (matching Milestone 21-28's original
// hardcoded assumption) -- everything else goes through the real
// parsed fields. framebuffer_type==2 (EGA text) is never a linear
// pixel buffer and is always rejected.
//
// Returns 0 with every out-parameter filled (including *fmt) if a
// supported framebuffer was found, or -1 (with *fb_addr left at 0) if
// no framebuffer tag exists or its format isn't one this kernel
// supports -- callers must treat -1 exactly like "no framebuffer" and
// fall back to VGA text mode, never guess a format.
static int mb2_find_fb(uint64_t mb_info_addr,
    uint64_t* fb_addr, uint32_t* fb_width, uint32_t* fb_height,
    uint32_t* fb_pitch, pixfmt64_t* fmt)
{
    *fb_addr = 0;
    if (!mb_info_addr) return -1;

    uint32_t total = *(uint32_t*)(uintptr_t)mb_info_addr;
    uint8_t* p   = (uint8_t*)(uintptr_t)(mb_info_addr + 8);
    uint8_t* end = (uint8_t*)(uintptr_t)(mb_info_addr + (uint64_t)total);

    while (p + 8 <= end) {
        uint32_t type = *(uint32_t*)p;
        uint32_t size = *(uint32_t*)(p + 4);
        if (type == 0) break; // end tag

        if (type == 8) {
            if (size < 32) break;
            uint8_t bpp     = *(uint8_t*)(p + 28);
            uint8_t fb_type = *(uint8_t*)(p + 29);
            // Bytes 30-31 are a u16 `reserved` field (NOT a single u8),
            // per the Multiboot2 spec -- color_info starts at offset 32.

            uint8_t rp, rs, gp, gs, bp_, bs;
            if (fb_type == 1 && size >= 38) {
                rp  = *(uint8_t*)(p + 32); rs = *(uint8_t*)(p + 33);
                gp  = *(uint8_t*)(p + 34); gs = *(uint8_t*)(p + 35);
                bp_ = *(uint8_t*)(p + 36); bs = *(uint8_t*)(p + 37);
            } else if (fb_type == 0 && bpp == 32) {
                // Legacy QEMU/Bochs mislabel -- see header comment.
                rp = 16; rs = 8; gp = 8; gs = 8; bp_ = 0; bs = 8;
            } else {
                break; // type 2 (EGA text), or an indexed mode we can't assume a layout for
            }

            if (pixfmt64_from_mb2(bpp, rp, rs, gp, gs, bp_, bs, fmt) < 0) break;

            *fb_addr   = *(uint64_t*)(p + 8);
            *fb_pitch  = *(uint32_t*)(p + 16);
            *fb_width  = *(uint32_t*)(p + 20);
            *fb_height = *(uint32_t*)(p + 24);
            return 0;
        }

        // Advance to next tag (each tag is 8-byte aligned)
        uint32_t skip = (size + 7u) & ~7u;
        if (!skip) break;
        p += skip;
    }
    return -1;
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
#ifdef PIXFMT64_RUN_TESTS
    klog_hex("pixfmt64_selftest: all passed = ", (uint32_t)pixfmt64_selftest());
#endif

    // Milestone 21: parse framebuffer info from multiboot2 before any output
    // so that boot diagnostics appear on the framebuffer when available.
    uint64_t fb_addr = 0;
    uint32_t fb_width = 0, fb_height = 0, fb_pitch = 0;
    pixfmt64_t fb_fmt;
    mb2_find_fb(mb_info_addr, &fb_addr, &fb_width, &fb_height, &fb_pitch, &fb_fmt);
    klog_hex("fb_addr:", (uint32_t)fb_addr);
    // physmem64_init MUST come before console64_init: the framebuffer
    // mapping (display64_init -> physmem64_map_mmio) calls
    // physmem64_alloc_page for its own PD table pages, which only works
    // once physmem64_init has parsed the memory map and built its
    // managed regions.
    uint64_t fb_size = (uint64_t)fb_height * (uint64_t)fb_pitch;
    physmem64_init(mb_info_addr, fb_addr, fb_size);
#ifdef PHYSMEM64_RUN_TESTS
    klog_hex("physmem64_selftest: all passed = ", (uint32_t)physmem64_selftest());
#endif
    heap64_init();
#ifdef HEAP64_RUN_TESTS
    klog_hex("heap64_selftest: all passed = ", (uint32_t)heap64_selftest());
#endif

#ifdef RUST64_SELFTEST_RUN
    {
        // Milestone 31: prove the Rust self-test leaves zero heap/
        // physical-page drift, the same before/after-snapshot pattern
        // every C self-test in this codebase already uses.
        heap64_stats_t hbefore, hafter;
        physmem64_stats_t pbefore, pafter;
        heap64_stats(&hbefore);
        physmem64_stats(&pbefore);

        klog_hex("toxenos_rust_selftest: fail_mask = ", (uint32_t)toxenos_rust_selftest());

        heap64_stats(&hafter);
        physmem64_stats(&pafter);
        int no_drift = hafter.used_bytes == hbefore.used_bytes &&
                        hafter.span_count == hbefore.span_count &&
                        pafter.used_pages == pbefore.used_pages &&
                        pafter.free_pages == pbefore.free_pages;
        klog(no_drift ? "toxenos_rust_selftest: no heap/physical-page drift PASS\n"
                       : "toxenos_rust_selftest: no heap/physical-page drift FAIL\n");
    }
#endif

    console64_init(fb_addr, fb_width, fb_height, fb_pitch, &fb_fmt);

#ifdef DISPLAY64_RUN_TESTS
    klog_hex("display64_selftest: all passed = ", (uint32_t)display64_selftest());
#endif

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

    out_line("physmem64 initialized");
    out_line("heap64 initialized");

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

    // Milestone 24: must come before pic_remap()/sti -- on real UEFI/APIC
    // hardware the 8259 PIC's output often isn't wired to the CPU by
    // default, so without this, no IRQ (timer or keyboard) would ever
    // arrive after `sti`. QEMU's default machine leaves PIC routing
    // intact regardless, so this has no observable effect there.
    lapic64_virtual_wire_init();

    pic_remap();
    irq64_register(0, timer64_handler);
    irq64_register(1, keyboard64_handler);
    out_line("PIC remapped, IRQ0/IRQ1 registered");

    // Milestone 29: bring up the shared 8042 controller (flush, enable
    // both ports + their IRQs in the configuration byte) before the
    // mouse driver tries to talk to the auxiliary port, and before
    // `sti` below -- same "no IRQ work happens until interrupts are
    // actually enabled" ordering IRQ0/IRQ1 already followed. The input
    // event queue must exist before either IRQ1 or IRQ12 could
    // possibly fire.
    input64_init();
    ps2_64_init();
    mouse64_init(); // registers+unmasks IRQ12 itself; a no-op failure if no mouse responds
    out_line("PS/2 controller + mouse initialized");

#ifdef DISPLAY_INPUT64_DUMP
    display64_dump();
    mouse64_dump();
    input64_dump();
#endif

    // Milestone 24: 100Hz instead of the legacy unprogrammed ~18.2Hz --
    // see include/process64.h's PROCESS64_TICK_DIVISOR for how this
    // maps to the scheduler's quantum.
    timer64_init(100);
    process64_init();
    service64_init(); // Milestone 32: must precede the first process64_spawn() below

    __asm__ volatile ("sti");
    out_line("Interrupts enabled (sti) -- timer at 100Hz, scheduler ready");

    // Milestone 28: PCI enumeration and every storage driver's MMIO
    // mapping/DMA setup MUST happen here, strictly before the first
    // process64_spawn() call below -- see physmem64_map_mmio()'s own
    // ordering requirement (paging64_create_as() copies the shared
    // direct-map entries into each new process's page tables BY VALUE
    // at creation time, not by reference, so a process created before a
    // given MMIO mapping exists would never see it).
    //
    // Registration order (AHCI/NVMe/VirtIO-blk via PCI, then ATA last)
    // is also the order kernel/vfs64.c's root-device selection tries
    // devices in -- ATA is deliberately tried last: it is the universal
    // legacy fallback (always at a fixed ISA port, needs no PCI
    // enumeration to find), so if a PCI storage controller's disk is
    // what actually has the TxFS64 filesystem on it, that is preferred
    // over an unrelated ATA drive coincidentally also present.
    pci64_enumerate();

#ifdef RUST64_PCI_DEMO_RUN
    {
        pci64_device_t* first = pci64_iter(0);
        if (first) {
            toxenos_pci_info_t info;
            pci64_fill_rust_info(first, &info);
            toxenos_rust_pci_demo(&info);
        } else {
            out_line("RUST64_PCI_DEMO_RUN: no PCI device found");
        }
    }
#endif

    ahci64_init();
    nvme64_init();
    virtio_blk64_init();

    ata64_init();
    ata64_register_blockdev();
    out_line("Storage drivers initialized");

#ifdef STORAGE64_DUMP
    pci64_dump();
    blockdev64_dump();
#endif

    if (vfs64_init_root_txfs() == 0) {
        out_line("TxFS64 mounted");
        vfs64_node_t node;
        if (vfs64_lookup("/hello.ts", &node) == 0 && !node.is_dir) {
            uint8_t buf[512];
            int n = vfs64_read(&node, 0, buf, sizeof(buf) - 1);
            buf[n > 0 ? (uint32_t)n : 0] = 0;
            out_kv("/hello.ts size: ", (uint64_t)n);
            klog((char*)buf);
            klog("\n");
        } else {
            out_line("/hello.ts: open failed");
        }
    } else {
        out_line("TxFS64 mount failed (bad magic)");
    }

#ifdef TXFS64_RUN_TESTS
    klog_hex("txfs64_selftest: all passed = ", (uint32_t)txfs64_selftest());
#endif

#ifdef PROCESS64_RUN_TESTS
    klog_hex("process64_selftest: all passed = ", (uint32_t)process64_selftest());
#endif

#ifdef USERVM64_RUN_TESTS
    klog_hex("uservm64_selftest: all passed = ", (uint32_t)uservm64_selftest());
#endif

#ifdef PIPE64_RUN_TESTS
    klog_hex("pipe64_selftest: all passed = ", (uint32_t)pipe64_selftest());
#endif

#ifdef SHM64_RUN_TESTS
    klog_hex("shm64_selftest: all passed = ", (uint32_t)shm64_selftest());
#endif

#ifdef VFS64_RUN_TESTS
    klog_hex("vfs64_selftest: all passed = ", (uint32_t)vfs64_selftest());
#endif

#ifdef SERVICE64_RUN_TESTS
    klog_hex("service64_selftest: all passed = ", (uint32_t)service64_selftest());
#endif

// Milestone 29: spawns /display_test64.nex64 (self-contained, no
// external input needed -- see user64/display_test64.c) from
// kernel/idle context, same process64_spawn+process64_wait pattern
// EXEC64_TEST_RUN below uses. Requires `make populate PACKAGE_DEBUG64=1`.
#ifdef DISPLAY_TEST64_RUN
    {
        uint32_t pid = 0;
        int code = -1;
        if (process64_spawn("/display_test64.nex64", 0, 0, &pid) == 0) code = process64_wait(pid);
        out_kv("display_test64: exit code = ", (uint64_t)(int64_t)code);
    }
#endif

// Milestone 29: spawns /input_test64.nex64, which blocks on real
// keyboard/mouse events (see user64/input_test64.c) -- drive it via
// QEMU monitor `sendkey`/`mouse_move`/`mouse_button` while this is
// running, or interactively. Requires `make populate PACKAGE_DEBUG64=1`.
#ifdef INPUT_TEST64_RUN
    {
        uint32_t pid = 0;
        int code = -1;
        if (process64_spawn("/input_test64.nex64", 0, 0, &pid) == 0) code = process64_wait(pid);
        out_kv("input_test64: exit code = ", (uint64_t)(int64_t)code);
    }
#endif

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
    // stale state left over from the first. Spawn+wait from the
    // kernel/idle context (parent_pid 0) since kernel_main64 is not
    // itself a tracked process.
    const char* exec64_test_path = "/exec64_test.nex64";
    for (int run = 1; run <= 2; run++) {
        out_kv("process64: run #", (uint64_t)run);
        klog(exec64_test_path);
        klog("\n");
        uint32_t pid = 0;
        int code = -1;
        if (process64_spawn(exec64_test_path, 0, 0, &pid) == 0) {
            code = process64_wait(pid);
        }
        out_kv("process64: returned to kernel, exit code: ", (uint64_t)(int64_t)code);
    }
    // Falls into the idle loop below with zero processes resident --
    // proves control genuinely returned to the kernel either way.
#elif defined(KERNEL64_DIAG_ONLY)
    // Falls straight into the idle loop below -- no userland, no VGA
    // clear, diagnostics stay on screen.
#elif defined(RUST64_PANIC_TEST_RUN)
    // Milestone 31: deliberately panics inside Rust code to prove
    // rust/toxenos_rs/src/panic.rs reports useful diagnostics via klog()
    // and then reaches the exact same kernel64_halt_forever() path a
    // C-side unhandled exception uses. Never returns -- boot stops here
    // by design, exactly like RING3_TEST64_RUN/KERNEL64_DIAG_ONLY above.
    toxenos_rust_panic_test();
#elif defined(RUST64_ALLOC_FAIL_TEST_RUN)
    // Milestone 31: observes stable no_std Rust's actual out-of-memory
    // behavior against an impossible allocation request. Never returns.
    toxenos_rust_alloc_fail_test();
#else
    // Milestone 13: normal interactive boot. One last line on the
    // diagnostics screen, then clear it before init64/shell64 ever get
    // a chance to print anything -- the user should land on a clean
    // shell prompt, not a wall of boot text (which is still fully
    // logged to klog/serial regardless).
    out_line("Launching ToxenOS64 interactive shell...");
    console64_clear();
    uint32_t init_pid = 0;
    if (process64_spawn("/init64.nex64", 0, 0, &init_pid) < 0) {
        klog("process64: failed to spawn /init64.nex64\n");
    }
    // Does NOT block here -- init64 is left READY, and the very next
    // timer tick's process64_tick() (or the keyboard IRQ, indirectly)
    // picks it up. Falling through to the idle loop below IS the
    // scheduler's fallback path, not a "nothing left to do" halt: the
    // moment any process is READY, the next tick switches away from it.
#endif

    // Milestone 24: this is no longer just an inert parking loop -- it
    // is the scheduler's own idle path, resumed via context_switch64
    // (kernel/process64.c) whenever no real process is READY. `sti`
    // ensures the next timer/keyboard IRQ actually arrives; `hlt` avoids
    // busy-looping while genuinely idle (e.g. between the last process
    // exiting and the machine being told to do anything else).
    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}
