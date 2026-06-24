// ToxenOS/kernel/kernel.c
#include <stdint.h>
#include "../include/memmap.h"
#include "../include/klog.h"
#include "../include/keyboard.h"
#include "../include/idt.h"
#include "../include/vga.h"
#include "../include/timer.h"
#include "../include/pic.h"
#include "../include/mm.h"
#include "../include/process.h"
#include "../include/syscall.h"
#include "../include/paging.h"
#include "../include/tss.h"
#include "../include/ring3.h"
#include "../include/vfs.h"
#include "../include/tmpfs.h"
#include "../include/ata.h"
#include "../include/txfs.h"
#include "../include/fat.h"
#include "../include/ext2.h"
#include "../include/elf.h"
#include "../include/tty.h"
#include "../include/framebuffer.h"
#include "../include/font.h"
#include "../include/fbterm.h"
#include "../include/pci.h"
#include "../include/e1000.h"
#include "../include/pmm.h"
#include "../include/net.h"
#include "../include/dhcp.h"
#include "../include/ahci.h"
#include "../include/nvme.h"
#include "../include/virtio_blk.h"
#include "../include/usb_hid.h"
#include "../include/cpu.h"
#include "../include/acpi.h"

// ── VGA legacy state (referenced by fbterm layer) ────────────────────────────
uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;
int     cursor_x      = 0;
int     cursor_y      = 0;
int     prompt_end_x  = 0;
uint8_t current_color = 0x07;

// ── Installer: bootable disk.img (GRUB prefix + TxFS) passed as ramdisk ──────
// Set when the GRUB-loaded module is a bootable disk.img rather than raw TxFS.
// SYS_INSTALL_CHUNK reads from here so it copies the full bootable image.
uint8_t*  install_img_buf  = 0;
uint32_t  install_img_size = 0;

extern uint32_t stack_top;
extern uint32_t kernel_directory[1024];

// ── LAPIC virtual-wire mode ───────────────────────────────────────────────────
// On UEFI systems the 8259 PIC is often fully masked (0xFF) by firmware and
// LAPIC LINT0 may not be in ExtINT mode.  Without this setup, no 8259 interrupt
// (timer IRQ 0, keyboard IRQ 1) ever reaches the CPU after 'sti'.
//
// We configure the LAPIC so the 8259's INT output is forwarded to the CPU via
// LINT0 in ExtINT delivery mode — the same "virtual wire mode" described in the
// Intel MP specification.
//
// Physical LAPIC base = 0xFEE00000 (default on all APIC-capable x86 systems).
// PDE for that address = 1015 (in kernel PDE range 768..1023), so the mapping
// is automatically present in every user-process page directory.

#define LAPIC_BASE  0xFEE00000u

static void lapic_virtual_wire_init(void)
{
    // Map LAPIC MMIO as cache-disable so MMIO reads/writes bypass the L1/L2 cache.
    // paging_init() pre-allocated the page table for PDE 1015; we just update
    // the one PTE for 0xFEE00000 to point at the real LAPIC physical address.
    paging_map(kernel_directory, LAPIC_BASE, LAPIC_BASE,
               PAGE_PRESENT | PAGE_WRITABLE | PAGE_CD);
    __asm__ volatile("invlpg (%0)" :: "r"(LAPIC_BASE) : "memory");

    volatile uint32_t* lapic = (volatile uint32_t*)LAPIC_BASE;

    // SVR (offset 0x0F0): software-enable LAPIC, spurious vector = 0xFF.
    // Bit 8 = APIC Software Enable.  Spurious vector 0xFF has bit 4 set,
    // which some early implementations require.
    lapic[0x0F0 / 4] = 0x1FFu;

    // TPR (offset 0x080): task priority 0 — accept all interrupt priorities.
    lapic[0x080 / 4] = 0;

    // LVT LINT0 (offset 0x350): ExtINT delivery, edge-triggered, unmasked.
    // Delivery mode 111 = ExtINT: on each timer tick the CPU does an INTA
    // cycle to the 8259 PIC which replies with the interrupt vector (0x20).
    lapic[0x350 / 4] = 0x700u;

    // LVT LINT1 (offset 0x360): NMI delivery, edge-triggered, unmasked.
    lapic[0x360 / 4] = 0x400u;

    klog("LAPIC: virtual wire mode (LINT0=ExtINT)\n");
}

// ── Terminal output helpers ───────────────────────────────────────────────────
void put_char(char c)     { fbterm_putchar(c); }
void erase_char()         { fbterm_erase(); }
void print(const char* s) { for (int i = 0; s[i]; i++) fbterm_putchar(s[i]); }
void clear_screen()       { fbterm_clear(); fbterm_draw_indicator(); }
void set_color(uint8_t c) { current_color = c; fbterm_set_color(c); }

void print_hex(uint32_t val) {
    const char* h = "0123456789ABCDEF";
    char buf[9]; buf[8] = 0;
    for (int i = 7; i >= 0; i--) { buf[i] = h[val & 0xF]; val >>= 4; }
    print(buf);
}

// ── Embedded user binaries ────────────────────────────────────────────────────
extern uint8_t _binary_build_user_init_elf_start[];
extern uint8_t _binary_build_user_init_elf_end[];

// ── Multiboot2 framebuffer tag (type 8) ───────────────────────────────────────
// Multiboot2 spec: framebuffer_addr is uint64_t at tag+8.
// On a 32-bit OS we can only use addresses below 4 GB (high 32 bits == 0).
typedef struct {
    uint32_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
} fb_info_t;

static fb_info_t parse_multiboot_fb(uint32_t mb_info_addr)
{
    fb_info_t info = {0, 0, 0, 0, 0};
    uint32_t total = *(uint32_t*)mb_info_addr;
    uint8_t* tag   = (uint8_t*)(mb_info_addr + 8);
    uint8_t* end   = (uint8_t*)(mb_info_addr + total);

    while (tag < end) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag + 4);
        if (type == 8) {
            uint32_t addr_lo = *(uint32_t*)(tag + 8);
            uint32_t addr_hi = *(uint32_t*)(tag + 12); // high 32 bits of uint64_t
            if (addr_hi == 0) {                        // only usable in 32-bit mode
                info.addr   = addr_lo;
                info.pitch  = *(uint32_t*)(tag + 16);
                info.width  = *(uint32_t*)(tag + 20);
                info.height = *(uint32_t*)(tag + 24);
                info.bpp    = *(uint8_t*) (tag + 28);
            }
            // addr_hi != 0 → framebuffer above 4 GB, leave info zeroed → VGA fallback
            break;
        }
        tag += (size + 7) & ~7;
    }
    return info;
}

// ── fb_setup: map framebuffer and bring up the terminal ───────────────────────
//
// The framebuffer physical address from UEFI GOP can be anywhere in the 4GB
// address space (e.g. 0xA0000000 on AMD systems). If we identity-map it at that
// physical address, it lands in a PDE < 768 (user-space PDE range). User process
// page directories only copy PDEs 768..1023 from kernel_directory, so they don't
// inherit user-range PDEs. When the timer ISR fires and fbterm_tick() writes to
// the framebuffer while a user process's CR3 is loaded, it page-faults.
//
// Fix: always map the framebuffer at a fixed kernel VA (0xFD000000, PDE 1012).
// This VA is in the kernel PDE range (768..1023), which all process page
// directories share via the pre-allocated kernel_tables. The timer ISR can then
// safely write to the framebuffer from any process context.

#define FB_KERN_VIRT  0xFD000000u  // fixed kernel VA for framebuffer (PDE 1012)

static void fb_setup(const fb_info_t* fb)
{
    if (!fb->addr || !fb->width || !fb->height) {
        fb_init(0, 0, 0, 0, 0);
        fbterm_init();
        fbterm_draw_indicator();
        return;
    }

    uint32_t phys     = fb->addr;
    uint32_t size     = fb->pitch * fb->height;
    uint32_t phys_pg  = phys & ~0xFFFu;
    uint32_t phys_off = phys - phys_pg;
    uint32_t pages    = (size + phys_off + 0xFFFu) / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++)
        paging_map(kernel_directory,
                   FB_KERN_VIRT + i * PAGE_SIZE,
                   phys_pg    + i * PAGE_SIZE,
                   PAGE_PRESENT | PAGE_WRITABLE);

    // Flush TLB: paging_map does not invlpg; a CR3 reload flushes everything.
    uint32_t cr3;
    __asm__ volatile("mov %%cr3,%0\n\t mov %0,%%cr3" : "=r"(cr3) :: "memory");

    fb_init(FB_KERN_VIRT + phys_off, fb->width, fb->height, fb->pitch, fb->bpp);
    fbterm_init();

    // Welcome banner removed — shown only on login screen (user/init.c)
    fbterm_draw_indicator();
}

// ── mount_detected_drives: probe drives 1-3 and mount detected filesystems ────
// Drive 0 (primary master) is always /C: (TxFS), already mounted before this.
// Drives 1-3 are checked for FAT, ext2, and TxFS in that order.
// Detected drives are mounted as /D:, /E:, /F:, /G:.
//
// NOTE: FS probing logic should eventually move into each driver as a probe()
// callback (see Phase 4). For now it lives here so the detection rationale
// is in one visible place.
// FS detection magic numbers and BPB offsets
// ATA_SECTOR_SIZE also defined here for the probe buffer size
#define ATA_SECTOR_SIZE  512
#define EXT2_MAGIC       0xEF53u
// TXFS_MAGIC comes from include/txfs.h
#define FAT_BPB_BPS_OFF  11
#define FAT_BPB_SPC_OFF  13
#define FAT_BPB_NFAT_OFF 16
#define EXT2_SB_MAGIC_OFF 56

static void mount_detected_drives(void)
{
    static uint8_t probe_buf[ATA_SECTOR_SIZE];
    char drive_letter[4] = "/D:";

    for (uint8_t drv = 1; drv <= 3; drv++) {
        fs_driver_t* detected = 0;

        // FAT: BPB is at LBA 0 on real hardware; QEMU virtual drives have it at
        // LBA 1 due to a quirk in how QEMU's disk image emulation works.
        // Try LBA 0 first (standard), then LBA 1 (QEMU fallback).
        int fat_ok = -1;
        for (uint32_t fat_lba = 0; fat_lba <= 1 && !detected; fat_lba++) {
            fat_ok = ata_read_drive(drv, fat_lba, probe_buf, 1);
            if (fat_ok == 1) {
                uint16_t bps  = (uint16_t)(probe_buf[FAT_BPB_BPS_OFF] |
                                ((uint16_t)probe_buf[FAT_BPB_BPS_OFF + 1] << 8));
                uint8_t  spc  = probe_buf[FAT_BPB_SPC_OFF];
                uint8_t  nfat = probe_buf[FAT_BPB_NFAT_OFF];
                if (bps == ATA_SECTOR_SIZE && spc != 0 && (nfat == 1 || nfat == 2))
                    detected = fat_init();
            }
        }

        // ext2: superblock at LBA 2 (byte offset 1024), magic at offset 56
        // Only probe if the drive responded to the FAT read (avoids CD/empty slots)
        if (!detected && fat_ok != -1) {
            if (ata_read_drive(drv, 2, probe_buf, 1) == 1) {
                uint16_t magic = (uint16_t)(probe_buf[EXT2_SB_MAGIC_OFF] |
                                 ((uint16_t)probe_buf[EXT2_SB_MAGIC_OFF + 1] << 8));
                if (magic == EXT2_MAGIC)
                    detected = ext2_init();
            }
        }

        // TxFS: superblock at LBA 8 (block 1 with 4096-byte blocks), magic at offset 0
        if (!detected && ata_read_drive(drv, 8, probe_buf, 1) == 1) {
            uint32_t magic = (uint32_t)(probe_buf[0]        |
                             ((uint32_t)probe_buf[1] <<  8) |
                             ((uint32_t)probe_buf[2] << 16) |
                             ((uint32_t)probe_buf[3] << 24));
            if (magic == TXFS_MAGIC)
                detected = txfs_init();
        }

        if (detected) {
            // Device string format: "N:/X:" where N=drive number, /X:=mountpoint
            char drv_str[8];
            drv_str[0] = (char)('0' + drv);
            drv_str[1] = ':';
            drv_str[2] = drive_letter[0];
            drv_str[3] = drive_letter[1];
            drv_str[4] = drive_letter[2];
            drv_str[5] = 0;
            vfs_mount(drive_letter, detected, drv_str);
            klog("Mounted drive\n");
            drive_letter[1]++;  // D -> E -> F -> G
        }
    }
}

// ── launch_init: load the embedded init ELF and jump to ring 3 ───────────────
// This is the point of no return — kernel_main never returns after this.
//
// Intentionally ELF-only, unlike the rest of userland: this is a standalone
// bootstrap loader that hand-parses program headers directly into
// kernel_directory before per-process page directories even exist. It does
// not go through process.c's load_binary_into_dir() magic-sniffing
// dispatcher, so it has no NEX support. Duplicating a second tiny parser
// here for one binary isn't worth it — init.elf stays ELF-pinned by design.
static void launch_init(void)
{
    uint8_t*  elf_buf  = _binary_build_user_init_elf_start;
    uint32_t  elf_size = (uint32_t)(_binary_build_user_init_elf_end
                                    - _binary_build_user_init_elf_start);
    (void)elf_size;

    if (*(uint32_t*)elf_buf != 0x464C457Fu) {
        print("PANIC: embedded init ELF is corrupt\n");
        while (1) __asm__ volatile("hlt");
    }

    uint32_t entry     = *(uint32_t*)(elf_buf + 24);
    uint32_t phoff     = *(uint32_t*)(elf_buf + 28);
    uint16_t phentsize = *(uint16_t*)(elf_buf + 42);
    uint16_t phnum     = *(uint16_t*)(elf_buf + 44);

    // Map PT_LOAD segments into kernel_directory (TTY0 context, no separate dir)
    for (int i = 0; i < phnum; i++) {
        uint8_t* ph     = elf_buf + phoff + i * phentsize;
        uint32_t type   = *(uint32_t*)(ph +  0);
        uint32_t offset = *(uint32_t*)(ph +  4);
        uint32_t vaddr  = *(uint32_t*)(ph +  8);
        uint32_t filesz = *(uint32_t*)(ph + 16);
        uint32_t memsz  = *(uint32_t*)(ph + 20);
        uint32_t flags  = *(uint32_t*)(ph + 24);
        if (type != 1 || memsz == 0) continue;

        uint32_t pf = PAGE_PRESENT | PAGE_USER;
        if (flags & 2) pf |= PAGE_WRITABLE;

        uint32_t page_start = vaddr & ~0xFFFu;
        uint32_t page_end   = (vaddr + memsz + 0xFFFu) & ~0xFFFu;

        for (uint32_t va = page_start; va < page_end; va += PAGE_SIZE) {
            uint32_t phys = paging_alloc_page();
            paging_map(kernel_directory, va, phys, pf);
            uint8_t* dst = (uint8_t*)KPHYS_TO_VIRT(phys);
            // Zero the page (handles BSS)
            for (int j = 0; j < (int)PAGE_SIZE; j++) dst[j] = 0;
            // Copy file bytes that fall within this page
            uint32_t seg_end = vaddr + filesz;
            for (uint32_t b = 0; b < PAGE_SIZE; b++) {
                uint32_t cur = va + b;
                if (cur < vaddr)    continue;
                if (cur >= seg_end) break;
                dst[b] = elf_buf[offset + (cur - vaddr)];
            }
        }
    }

    // Map user stack
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t va   = USER_STACK_TOP - (uint32_t)(i + 1) * PAGE_SIZE;
        uint32_t phys = paging_alloc_page();
        paging_map(kernel_directory, va, phys,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    process_retire_kernel(); // prevent scheduler from restoring PID 0's stale kernel ESP
    tss_set_kernel_stack((uint32_t)&stack_top);
    jump_to_ring3((void*)entry, USER_STACK_TOP - 4);
    // Never reached
}

// ── kernel_main ───────────────────────────────────────────────────────────────
void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    (void)magic;

    // mb_info_addr arrives as a physical address from boot.asm.
    // The kernel's high-half mapping covers physical 0x00000000..0x003FFFFF
    // at virtual 0xC0000000..0xC03FFFFF, so the multiboot info block
    // (which GRUB places in low RAM) is accessible via KPHYS_TO_VIRT.
    uint32_t mb_info_virt = KPHYS_TO_VIRT(mb_info_addr);

    fb_info_t fb = parse_multiboot_fb(mb_info_virt);

    // Locate the GRUB-loaded disk.img module (multiboot2 tag type 3).
    // Physical addresses only here — virtual access deferred until after paging_init.
    uint32_t ramdisk_phys = 0, ramdisk_bytes = 0;
    {
        uint32_t total = *(uint32_t*)mb_info_virt;
        uint8_t* tag   = (uint8_t*)(mb_info_virt + 8);
        uint8_t* end   = (uint8_t*)(mb_info_virt + total);
        while (tag < end) {
            uint32_t type = *(uint32_t*)tag;
            uint32_t size = *(uint32_t*)(tag + 4);
            if (type == 0) break;
            if (type == 3 && size >= 16) {
                uint32_t ms = *(uint32_t*)(tag + 8);
                uint32_t me = *(uint32_t*)(tag + 12);
                if (me > ms) { ramdisk_phys = ms; ramdisk_bytes = me - ms; }
                break;
            }
            tag += (size + 7) & ~7;
        }
    }

    // ── Phase 1: memory and paging ───────────────────────────────────────────
    pmm_init(mb_info_virt);
    paging_init();
    // Pass ramdisk end so heap starts after ramdisk — prevents heap from
    // overlapping the in-memory disk image and corrupting TxFS inode blocks.
    mm_init(ramdisk_phys && ramdisk_bytes ? ramdisk_phys + ramdisk_bytes : 0);

    // ── Phase 2: core hardware ───────────────────────────────────────────────
    idt_init();
    lapic_virtual_wire_init();  // must come before pic_remap() and sti
    pic_remap();
    timer_init(100);
    keyboard_init();

    // ── Phase 3: kernel subsystems ───────────────────────────────────────────
    process_init();
    syscall_init();
    tss_init((uint32_t)&stack_top);
    // Enable interrupts only after TSS and process table are ready
    __asm__ volatile("sti");
    vfs_init();
    klog("vfs_mount /\n");
    vfs_mount("/", tmpfs_init(), 0);
    klog("ToxenOS kernel started\n");
    cpu_detect();

    // ── Phase 4: display ─────────────────────────────────────────────────────
    tty_init();
    fb_setup(&fb);

    // ── Phase 5: storage ─────────────────────────────────────────────────────
    // Priority: NVMe → AHCI → ATA PIO → ramdisk (GRUB module fallback).
    //
    print("Detecting storage...");
    // TxFS probe checks two offsets:
    //  offset 0:    raw TxFS image (direct installation or QEMU NVMe with disk.img)
    //  offset 2048: installed layout — disk.img has 1MB GRUB zone at the front,
    //               TxFS starts at sector 2048 (superblock at 2048+8=2056)
    //
    // When the ramdisk module is a bootable disk.img (GRUB prefix + TxFS), we:
    //  - Keep the full module as install_img_buf for the installer
    //  - Point the TxFS ramdisk at the TxFS portion (skipping the 1MB prefix)
    pci_init();   // must come before NVMe/AHCI which scan pci_devices[]
    ata_init();
    {
        static uint8_t probe[512];
        int found = 0;

        #define TXFS_MAGIC_CHECK(b) \
            ((uint32_t)((b)[0] | ((uint32_t)(b)[1]<<8) | \
                        ((uint32_t)(b)[2]<<16) | ((uint32_t)(b)[3]<<24)) == TXFS_MAGIC)

        // TXFS_PROBE_AT(offset): probe for TxFS at LBA (offset + 8)
        // offset=0     → raw TxFS image (QEMU direct or old format)
        // offset=10240 → GPT layout (current: GPT+core.img+FAT ESP at LBA 2048)
        // offset=8704  → MBR layout (previous: MBR+core+FAT at LBA 512)
        // offset=2048  → old 1 MB GRUB-only prefix (kept for compatibility)
        #define TXFS_PROBE_AT(off) ( \
            ata_read((off) + 8, probe, 1) == 1 && TXFS_MAGIC_CHECK(probe))

        // Helper: find TxFS on a single device across all known offsets
        #define PROBE_DEVICE(tag_none, tag_gpt, tag_mbr, tag_old) \
            if      (TXFS_PROBE_AT(0))     { found=1; klog(tag_none); } \
            else if (TXFS_PROBE_AT(10240)) { found=1; txfs_set_lba_offset(10240); klog(tag_gpt); } \
            else if (TXFS_PROBE_AT(8704))  { found=1; txfs_set_lba_offset(8704);  klog(tag_mbr); } \
            else if (TXFS_PROBE_AT(2048))  { found=1; txfs_set_lba_offset(2048);  klog(tag_old); }

        if (!found && nvme_init() == 0) {
            ata_set_nvme_ready(1);
            ata_set_nvme(1);
            PROBE_DEVICE("TxFS: NVMe\n", "TxFS: NVMe (gpt)\n", "TxFS: NVMe (mbr)\n", "TxFS: NVMe (old)\n")
            else { ata_set_nvme(0); }
        }
        if (!found && ahci_init() == 0) {
            ata_set_ahci_ready(1);
            ata_set_ahci(1);
            PROBE_DEVICE("TxFS: AHCI\n", "TxFS: AHCI (gpt)\n", "TxFS: AHCI (mbr)\n", "TxFS: AHCI (old)\n")
            else { ata_set_ahci(0); }
        }
        if (!found && virtio_blk_init() == 0) {
            ata_set_virtio_ready(1);
            ata_set_virtio(1);
            PROBE_DEVICE("TxFS: VirtIO\n", "TxFS: VirtIO (gpt)\n", "TxFS: VirtIO (mbr)\n", "TxFS: VirtIO (old)\n")
            else { ata_set_virtio(0); }
        }
        if (!found) {
            PROBE_DEVICE("TxFS: ATA\n", "TxFS: ATA (gpt)\n", "TxFS: ATA (mbr)\n", "TxFS: ATA (old)\n")
        }

        // Always detect bootable ramdisk — set install_img_buf even when NVMe/AHCI
        // provides TxFS, so the installer can copy the full bootable image to disk.
        if (!install_img_buf && ramdisk_phys && ramdisk_bytes) {
            uint8_t* rd = (uint8_t*)KPHYS_TO_VIRT(ramdisk_phys);
            // GPT layout (current): TxFS at LBA 10240 → superblock at LBA 10248
            if (ramdisk_bytes > (10248 + 1) * 512 &&
                TXFS_MAGIC_CHECK(rd + 10248*512)) {
                install_img_buf  = rd;
                install_img_size = ramdisk_bytes;
            // MBR layout: TxFS at LBA 8704 → superblock at LBA 8712
            } else if (ramdisk_bytes > (8712 + 1) * 512 &&
                       TXFS_MAGIC_CHECK(rd + 8712*512)) {
                install_img_buf  = rd;
                install_img_size = ramdisk_bytes;
            // Old layout: TxFS at LBA 2048 → superblock at LBA 2056
            } else if (ramdisk_bytes > (2056 + 1) * 512 &&
                       TXFS_MAGIC_CHECK(rd + 2056*512)) {
                install_img_buf  = rd;
                install_img_size = ramdisk_bytes;
            }
        }
        if (!found && ramdisk_phys && ramdisk_bytes) {
            uint8_t* rd = (uint8_t*)KPHYS_TO_VIRT(ramdisk_phys);
            if (ramdisk_bytes >= 9*512 &&
                TXFS_MAGIC_CHECK(rd + 8*512)) {
                ata_set_ramdisk(rd, ramdisk_bytes);
                klog("TxFS: ramdisk\n");
            } else if (install_img_buf) {
                // Bootable disk.img — detect which layout to find TxFS offset
                uint32_t txfs_off;
                if (ramdisk_bytes > (10248+1)*512 && TXFS_MAGIC_CHECK(rd + 10248*512))
                    txfs_off = 10240;
                else if (ramdisk_bytes > (8712+1)*512 && TXFS_MAGIC_CHECK(rd + 8712*512))
                    txfs_off = 8704;
                else
                    txfs_off = 2048;
                ata_set_ramdisk(rd + txfs_off*512, ramdisk_bytes - txfs_off*512);
                klog("TxFS: ramdisk (bootable img)\n");
            } else {
                klog("TxFS: ramdisk has no recognised TxFS\n");
            }
        }
        if (!found && !ata_has_ramdisk()) {
            klog("TxFS: no storage found!\n");
        }

        #undef TXFS_PROBE_AT
        #undef TXFS_MAGIC_CHECK
    }
    klog("ATA initialised\n");
    print(" ok\n");
    vfs_mount("/C:", txfs_init(), 0);
    klog("Mounted /C: (TxFS)\n");
    {
        extern int vfs_stat(const char*, uint32_t*);
        uint32_t sz = 0;
        if (vfs_stat("/C:/BSM/SystemT/bmsg.nex", &sz) < 0)
            klog("WARN: bmsg.nex not found in /C:\n");
        else
            klog_hex("TxFS: bmsg.nex size=", sz);
    }
    mount_detected_drives();

    // ── Phase 6: ACPI + networking + USB ────────────────────────────────────
    print("ACPI init...");
    acpi_init();
    print(" ok\n");
    print("USB init...");
    usb_hid_init();
    print(" ok\n");
    print("Net init...");
    int nic_ok = (e1000_init() == 0);
    net_init();
    if (nic_ok) {
        if (!dhcp_run()) klog("DHCP failed — using static IP\n");
    } else {
        klog("No NIC — skipping DHCP\n");
    }
    print(" ok\n");

    // ── Phase 7: launch userspace ────────────────────────────────────────────
    klog("Launching init\n");
    clear_screen();
    launch_init();
}
