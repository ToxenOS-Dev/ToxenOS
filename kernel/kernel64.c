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
#include "../include/memobj64.h"
#include "../include/shm64.h"
#include "../include/gpu64.h"
#include "../include/blockdev64.h"
#include "../include/pci64.h"
#include "../include/pci_irq64.h"
#include "../include/virtio_irq64.h"
#include "../include/ahci64.h"
#include "../include/nvme64.h"
#include "../include/virtio_blk64.h"
#include "../include/virtio_pci64.h"
#include "../include/virtio_gpu64.h"
#include "../include/virtio_input64.h"
#include "../include/pixfmt64.h"
#include "../include/display64.h"
#include "../include/ps2_64.h"
#include "../include/input64.h"
#include "../include/mouse64.h"
#include "../include/service64.h"
#include "../include/rustffi64.h"
#include "../include/tsc64.h"

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

// M+1A: define to run the memobj64 self-test suite (kernel-only, no
// ring3 fixture needed -- unlike SHM64_RUN_TESTS below, this one has no
// PACKAGE_DEBUG64 dependency). Runs BEFORE SHM64_RUN_TESTS when both are
// enabled, since memobj64 is the lower layer shm64 now sits on top of.
// #define MEMOBJ64_RUN_TESTS 1

// Define to run the Milestone 26 shared-memory self-test suite, same
// timing requirements as PROCESS64_RUN_TESTS -- requires
// `make populate PACKAGE_DEBUG64=1` for shm_test64.nex64.
// #define SHM64_RUN_TESTS 1

// M+1B: define to run the gpu64 self-test suite (kernel-only, no ring3
// fixture needed -- registers its own private null/test driver). Runs
// AFTER SHM64_RUN_TESTS when both are enabled, since several of its own
// cases exercise a real shm64_t sharing backing with a gpu64_buffer_t.
// #define GPU64_RUN_TESTS 1

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

// Define to run the Milestone 32.1 compositor event-delivery
// regression test right after storage/display/input are up (same
// timing requirement as DISPLAY_TEST64_RUN/INPUT_TEST64_RUN below --
// needs real display/input hardware) -- see
// user64/compositor_stall_test64.c for the full scenario (a disposable
// test compositor instance, a normal client, a client that floods
// requests without ever reading replies, and a late-connecting client)
// and kernel/kernel64.c's own invocation below for the pass/fail +
// repeated-cycle leak check. Requires `make populate PACKAGE_DEBUG64=1`
// for compositor64_test.nex64/compositor_stall_test64.nex64.
// #define COMPOSITOR_STALL_TEST64_RUN 1

// Define to run the Milestone 33 ToxUI (userlib/toxui) self-test suite
// -- PNG decoding, drawing primitives, TrueType font/text rendering --
// right after storage/VFS are up (needs the VFS for both the test
// binary and its /toxui_test_assets/ PNG fixtures + the real
// DejaVuSans.ttf under /system_manager/system_data/display_interface/
// fonts/). No display/input/compositor needed -- entirely single-
// process computation. See user64/toxui_test64.c and
// userlib/toxui/tox_selftest.c for the full scenario. Requires
// `make populate PACKAGE_DEBUG64=1`.
// #define TOXUI_TEST64_RUN 1

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

// M+2: define to probe a modern (disable-legacy=on) VirtIO PCI device
// (PCI64_DEVICE_VIRTIO_GPU_MODERN -- see include/virtio_pci64.h's own
// comment on why this device ID, used for probing only, is never
// registered as a real GPU device) right after pci64_enumerate() and
// run the Rust-side transport self-test against it: full status/feature
// negotiation, one virtqueue set up, DRIVER_OK set. Touches real device
// state (unlike RUST64_PCI_DEMO_RUN above), so it must run here, before
// the first process64_spawn() -- see physmem64_map_mmio()'s own
// ordering requirement, identical to every storage driver below. A
// no-op (logs "not found") if no such device is attached; never
// interacts with kernel/virtio_blk64.c's own legacy device (a different
// PCI device ID entirely). Requires a real modern VirtIO PCI device
// under QEMU, e.g. `-device virtio-gpu-pci,disable-legacy=on`.
// #define VIRTIO_PCI64_TEST_RUN 1

// M+2: device-independent self-test (pure bounds-check arithmetic, no
// real hardware needed -- see kernel/virtio_pci64.c's own header
// comment). Safe to run on every boot, unlike VIRTIO_PCI64_TEST_RUN
// above.
// #define VIRTIO_PCI64_RUN_TESTS 1

// M+3/M+4: VirtIO-GPU probing is now UNCONDITIONAL (see this file's own
// "M+4: unconditional VirtIO-GPU probe" block below, right after
// pci64_enumerate()) -- no debug flag gates it any more, matching
// ahci64_init()/nvme64_init()/virtio_blk64_init()'s own "always
// attempt, no-op cleanly if absent" convention. A no-op (logs "not
// found") if no such device is attached -- the existing Multiboot
// framebuffer / display64 software path, and compositor64 on top of
// it, are completely untouched either way (this milestone's own "must
// not destroy the fallback" requirement). This IS still mutually
// exclusive with VIRTIO_PCI64_TEST_RUN above -- both would
// independently reset/renegotiate the SAME physical device, which a
// real device does not tolerate gracefully; never enable
// VIRTIO_PCI64_TEST_RUN on a boot that also has a real VirtIO-GPU
// device attached.
//
// The four flags below are now purely additional, opt-in DIAGNOSTICS
// layered on top of that always-on init -- not prerequisites for it.

// M+3: logs the unconditionally-initialized device's GET_DISPLAY_INFO
// result (every enabled scanout's geometry).
// #define VIRTIO_GPU64_DISPLAY_INFO_RUN 1

// M+3: runs the item-9 isolated scanout proof (kernel/virtio_gpu64.c's
// virtio_gpu64_run_scanout_test()) -- an obvious color-bars test
// pattern, driven through create/attach/transfer/set_scanout/flush.
// M+4: this now REFUSES (logs and returns NULL, changes nothing) once
// virtio_gpu64_init_compositor_backend() has already claimed scanout 0
// for the real compositor -- which, since that call is now
// unconditional, means this flag is effectively a no-op whenever a
// real VirtIO-GPU device is present. Kept only for the rare case of
// debugging the driver with the compositor backend deliberately not
// wired up; harmless to leave enabled otherwise.
// #define VIRTIO_GPU64_SCANOUT_TEST_RUN 1

// M+3: the full virtio_gpu64 self-test suite (device-dependent -- see
// kernel/virtio_gpu64.c's own header comment). M+4: its scanout-round-
// trip sub-case is SKIPPED, not failed, whenever the real compositor
// backend already owns scanout 0 -- see virtio_gpu64_selftest()'s own
// updated comment.
// #define VIRTIO_GPU64_RUN_TESTS 1

// M+3 item 15: logs a raw before/after tick-count comparison between the
// software present path and VirtIO-GPU's transfer+flush, for a full
// 1024x768 frame and a smaller 128x128 damaged region. Never touches
// scanout, so it's unaffected by whether the compositor backend is live.
// #define VIRTIO_GPU64_PERF_COMPARE_RUN 1

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

extern void timer64_init(uint32_t frequency);
extern void keyboard64_init(void); // M+7A: input64 device-model registration only

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

    // M+11A: interrupt core bring-up. Remaps + masks the 8259s, discovers
    // ACPI/MADT and decides PIC vs LAPIC+IOAPIC (transactionally, with
    // rollback), then each driver below requests its own IRQ. Replaces the
    // M24 lapic64_virtual_wire_init()+pic_remap()+irq64_register() sequence
    // (the PIC path still performs the virtual-wire setup internally).
    irq64_init(mb_info_addr);
    out_line("Interrupt core initialized");

    // Milestone 29: bring up the shared 8042 controller (flush, enable
    // both ports + their IRQs in the configuration byte) before the
    // mouse driver tries to talk to the auxiliary port, and before
    // `sti` below -- same "no IRQ work happens until interrupts are
    // actually enabled" ordering IRQ0/IRQ1 already followed. The input
    // event queue must exist before either IRQ1 or IRQ12 could
    // possibly fire.
    input64_init();
    keyboard64_init(); // M+7A input64 registration + M+11A IRQ1 request
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

    irq64_selftest_run(); // M+11A: deterministic interrupt-foundation tests (IF still 0)

    __asm__ volatile ("sti");
    out_line("Interrupts enabled (sti) -- timer at 100Hz, scheduler ready");

    // M+4 investigation: calibrate the RDTSC-based high-resolution clock
    // now that real PIT ticks are advancing (tsc64_init() busy-waits on
    // them). Needed because the 100Hz PIT tick's 10ms granularity could
    // not expose the sub-millisecond VirtIO-GPU command costs manual
    // testing found real interactive lag from.
    tsc64_init();

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

    // M+11B: deterministic MSI/MSI-X foundation tests (fake devices + a
    // private irq table; real devices are only inspected read-only), then --
    // if built with -DMSI64_EDU_TEST and a QEMU `edu` device is attached --
    // the real-MSI test. Both run before any process is spawned.
    pci_irq64_selftest_run();
    virtio_irq64_selftest_run();   // M+11C: kwait/resched/ownership/orchestration tests (kernel/virtio_irq64_selftest.c)
    msi64_edu_test_run();

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

#ifdef VIRTIO_PCI64_TEST_RUN
    {
        pci64_device_t* modern = pci64_find_device(PCI64_VENDOR_VIRTIO, PCI64_DEVICE_VIRTIO_GPU_MODERN, 0);
        if (!modern) {
            out_line("VIRTIO_PCI64_TEST_RUN: no modern VirtIO device found");
        } else {
            pci64_enable_device(modern);
            virtio_pci64_transport_info_t info;
            if (virtio_pci64_probe(modern, &info) < 0) {
                out_line("VIRTIO_PCI64_TEST_RUN: device has no vendor-specific PCI capabilities");
            } else {
                virtio_pci64_dump(&info);
                int32_t rc = toxenos_rust_virtio_pci_selftest(&info);
                out_kv("VIRTIO_PCI64_TEST_RUN: toxenos_rust_virtio_pci_selftest rc = ", (uint64_t)(int64_t)rc);
            }
        }
    }
#endif

    // M+4: unconditional VirtIO-GPU probe + real compositor-backend
    // wiring -- matches every other storage/PCI driver's own "always
    // attempt, no-op cleanly if absent" convention (ahci64_init/
    // nvme64_init/virtio_blk64_init below), no longer a manual debug-
    // only path (that was M+3's narrower scope: proving the driver in
    // isolation before anything depended on it). A failure at EITHER
    // step is silently safe: virtio_gpu64_init() logs and returns -1 if
    // no device exists or transport bring-up fails, exactly as always;
    // virtio_gpu64_init_compositor_backend() independently logs and
    // returns -1 WITHOUT touching display64 at all if display setup
    // fails after that -- display64 is simply left on whatever
    // framebuffer-or-none state it already had, and compositor64 (via
    // SYS64_DISPLAY_OPEN) has no idea which backend won.
    {
        int rc = virtio_gpu64_init();
        out_kv("virtio_gpu64_init rc = ", (uint64_t)(int64_t)rc);
        if (rc == 0) {
            int brc = virtio_gpu64_init_compositor_backend();
            out_kv("virtio_gpu64_init_compositor_backend rc = ", (uint64_t)(int64_t)brc);

            // M+7: unconditional, "always probe, no-op cleanly if
            // absent" convention, same as every other driver step here.
            // A failure (no cursor virtqueue, or any setup step) is NOT
            // fatal and does not affect display or compositor backend
            // state at all -- compositor64's own SYS64_CURSOR_AVAILABLE
            // query simply reports 0 and it keeps using its existing,
            // unmodified software cursor path.
            int crc = virtio_gpu64_init_cursor();
            out_kv("virtio_gpu64_init_cursor rc = ", (uint64_t)(int64_t)crc);

            // M+10: atomic display state backend, registered BEFORE the
            // M+7 cursor pixel self-test below -- display64_cursor_set_image()
            // (which that self-test calls, via the real
            // display64_cursor_set_image() entry point) now routes
            // through display64_state_check()/commit(), which requires
            // an atomic backend to validate/issue a CURSOR_IMAGE delta
            // (see display64_state_check()'s own §19 fallback rule).
            // Registering here first means every cursor/display64 self-
            // test below runs against the SAME fully-live pipeline real
            // clients will use later, not a stale pre-atomic window.
            int arc = virtio_gpu64_init_atomic_backend();
            out_kv("virtio_gpu64_init_atomic_backend rc = ", (uint64_t)(int64_t)arc);

            // Deterministic byte-level proof, no visual inspection
            // needed -- see this function's own header comment.
            virtio_gpu64_cursor_pixel_selftest();
        }

#ifdef VIRTIO_GPU64_DISPLAY_INFO_RUN
        if (rc == 0) {
            virtio_gpu64_display_mode_t modes[VIRTIO_GPU64_MAX_SCANOUTS];
            int enabled = virtio_gpu64_get_display_info(modes);
            out_kv("VIRTIO_GPU64_DISPLAY_INFO_RUN: enabled scanouts = ", (uint64_t)(int64_t)enabled);
            for (int i = 0; i < VIRTIO_GPU64_MAX_SCANOUTS; i++) {
                if (!modes[i].enabled) continue;
                out_kv("  scanout width  = ", modes[i].width);
                out_kv("  scanout height = ", modes[i].height);
            }
        }
#endif

#ifdef VIRTIO_GPU64_SCANOUT_TEST_RUN
        if (rc == 0) {
            gpu64_buffer_t* test_buf = virtio_gpu64_run_scanout_test(1024, 768);
            out_kv("VIRTIO_GPU64_SCANOUT_TEST_RUN: buffer created = ", (uint64_t)(test_buf != 0));
            // Deliberately never torn down here -- this debug-only flag
            // exists purely so the color-bars pattern stays on screen
            // for an external screenshot to capture (see this flag's own
            // header comment). `test_buf` intentionally goes unused past
            // this point; the driver keeps the resource/scanout live
            // regardless of what happens to this local variable, exactly
            // like every "left visible for manual inspection" debug flag
            // elsewhere in this file (e.g. DISPLAY64_RUN_TESTS's own
            // "scribbles over the screen" note).
            (void)test_buf;
        }
#endif

#ifdef VIRTIO_GPU64_RUN_TESTS
        if (rc == 0) {
            klog_hex("virtio_gpu64_selftest: all passed = ", (uint32_t)virtio_gpu64_selftest());
        }
#endif

#ifdef VIRTIO_GPU64_PERF_COMPARE_RUN
        if (rc == 0) {
            virtio_gpu64_perf_compare(1024, 768, 30);
            virtio_gpu64_perf_compare(128, 128, 2000);
        }
#endif
    }

    // M+7A: unconditional VirtIO-input probe -- same "always attempt,
    // no-op cleanly if absent" convention as virtio_gpu64_init() above.
    // A missing device, or one with no absolute axes, leaves PS/2 as the
    // sole pointer source exactly as before this milestone -- see
    // include/virtio_input64.h's own header comment.
    {
        int irc = virtio_input64_init();
        out_kv("virtio_input64_init rc = ", (uint64_t)(int64_t)irc);
        virtio_irq64_post_init_check();   // M+11C: ownership checks against the real, now-initialised devices
        virtio_irq64_report();            // M+11C: per-device interrupt-mode summary (BDF / MSI-X entry / irq / vector)
#ifdef VIRTIO_IRQ_TEST_DROP_CTL_IRQ
        virtio_gpu64_irq_test_drop_ctl(); // M+11C test build: lost-interrupt recovery + verified demotion
#endif
#ifdef VIRTIO_IRQ_TEST_DEMOTE_INPUT
        virtio_input64_irq_test_demote(); // M+11C test build: on-demand verified demotion of the input device
#endif
        // Pure arithmetic, no device required -- see this function's
        // own header comment. Always run, regardless of irc.
        virtio_input64_selftest();
    }

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

#ifdef MEMOBJ64_RUN_TESTS
    klog_hex("memobj64_selftest: all passed = ", (uint32_t)memobj64_selftest());
#endif

#ifdef SHM64_RUN_TESTS
    klog_hex("shm64_selftest: all passed = ", (uint32_t)shm64_selftest());
#endif

#ifdef GPU64_RUN_TESTS
    klog_hex("gpu64_selftest: all passed = ", (uint32_t)gpu64_selftest());
#endif

#ifdef VIRTIO_PCI64_RUN_TESTS
    klog_hex("virtio_pci64_selftest: all passed = ", (uint32_t)virtio_pci64_selftest());
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

// Milestone 32.1: see this file's own header comment on
// COMPOSITOR_STALL_TEST64_RUN. Runs the primary pass/fail scenario
// once, then 5 further full cycles (each spawning a disposable test
// compositor + 3 clients from scratch) purely to compare heap/
// physical-page stats before vs after -- same before/after-snapshot
// pattern every other repeated-cycle case in this codebase uses.
#ifdef COMPOSITOR_STALL_TEST64_RUN
    {
        #define COMPOSITOR_STALL_TEST_PATH "/compositor_stall_test64.nex64"
        uint32_t pid = 0;
        int code = -1;
        if (process64_spawn(COMPOSITOR_STALL_TEST_PATH, "", 0, &pid) == 0) code = process64_wait(pid);
        klog(code == 42 ? "compositor_stall_test64: PASS\n" : "compositor_stall_test64: FAIL\n");
        out_kv("compositor_stall_test64: exit code = ", (uint64_t)(int64_t)code);

        heap64_stats_t hbefore, hafter;
        physmem64_stats_t pbefore, pafter;
        heap64_stats(&hbefore);
        physmem64_stats(&pbefore);
        int cycles_ok = 1;
        for (int i = 0; i < 5; i++) {
            uint32_t cpid = 0;
            if (process64_spawn(COMPOSITOR_STALL_TEST_PATH, "", 0, &cpid) < 0) { cycles_ok = 0; break; }
            if (process64_wait(cpid) != 42) { cycles_ok = 0; break; }
        }
        heap64_stats(&hafter);
        physmem64_stats(&pafter);
        int no_drift = hafter.used_bytes == hbefore.used_bytes && hafter.span_count == hbefore.span_count &&
                        pafter.used_pages == pbefore.used_pages && pafter.free_pages == pbefore.free_pages;
        klog(cycles_ok ? "compositor_stall_test64: repeated cycles PASS\n" : "compositor_stall_test64: repeated cycles FAIL\n");
        klog(no_drift ? "compositor_stall_test64: no heap/physical-page drift PASS\n" : "compositor_stall_test64: no heap/physical-page drift FAIL\n");
    }
#endif

// Milestone 33: see this file's own header comment on TOXUI_TEST64_RUN.
// Same pattern as COMPOSITOR_STALL_TEST64_RUN above -- primary pass/
// fail once, then 5 further full cycles (each a fresh process running
// the ENTIRE tox_selftest suite, including its own internal repeated
// PNG/font load-free loops) purely for the outer heap/physical-page
// drift comparison.
#ifdef TOXUI_TEST64_RUN
    {
        #define TOXUI_TEST_PATH "/toxui_test64.nex64"
        uint32_t pid = 0;
        int code = -1;
        if (process64_spawn(TOXUI_TEST_PATH, "", 0, &pid) == 0) code = process64_wait(pid);
        klog(code == 42 ? "toxui_test64: PASS\n" : "toxui_test64: FAIL\n");
        out_kv("toxui_test64: exit code = ", (uint64_t)(int64_t)code);

        heap64_stats_t hbefore, hafter;
        physmem64_stats_t pbefore, pafter;
        heap64_stats(&hbefore);
        physmem64_stats(&pbefore);
        int cycles_ok = 1;
        for (int i = 0; i < 5; i++) {
            uint32_t cpid = 0;
            if (process64_spawn(TOXUI_TEST_PATH, "", 0, &cpid) < 0) { cycles_ok = 0; break; }
            if (process64_wait(cpid) != 42) { cycles_ok = 0; break; }
        }
        heap64_stats(&hafter);
        physmem64_stats(&pafter);
        int no_drift = hafter.used_bytes == hbefore.used_bytes && hafter.span_count == hbefore.span_count &&
                        pafter.used_pages == pbefore.used_pages && pafter.free_pages == pbefore.free_pages;
        klog(cycles_ok ? "toxui_test64: repeated cycles PASS\n" : "toxui_test64: repeated cycles FAIL\n");
        klog(no_drift ? "toxui_test64: no heap/physical-page drift PASS\n" : "toxui_test64: no heap/physical-page drift FAIL\n");
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
