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

// ── VGA legacy state (referenced by fbterm layer) ────────────────────────────
uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;
int     cursor_x      = 0;
int     cursor_y      = 0;
int     prompt_end_x  = 0;
uint8_t current_color = 0x07;

extern uint32_t stack_top;
extern uint32_t kernel_directory[1024];

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
            info.addr   = *(uint32_t*)(tag + 8);
            info.pitch  = *(uint32_t*)(tag + 16);
            info.width  = *(uint32_t*)(tag + 20);
            info.height = *(uint32_t*)(tag + 24);
            info.bpp    = *(uint8_t*) (tag + 28);
            break;
        }
        tag += (size + 7) & ~7;
    }
    return info;
}

// ── fb_setup: map framebuffer and bring up the terminal ───────────────────────
static void map_phys_range(uint32_t phys_start, uint32_t size)
{
    uint32_t start = phys_start & ~0xFFFu;
    uint32_t end   = (phys_start + size + 0xFFFu) & ~0xFFFu;
    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE)
        paging_map(kernel_directory, addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
}

static void fb_setup(const fb_info_t* fb)
{
    if (fb->addr && fb->width && fb->height)
        map_phys_range(fb->addr, fb->pitch * fb->height);

    fb_init(fb->addr, fb->width, fb->height, fb->pitch, fb->bpp);
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

    // ── Phase 1: memory and paging ───────────────────────────────────────────
    pmm_init(mb_info_virt);
    paging_init();
    mm_init();

    // ── Phase 2: core hardware ───────────────────────────────────────────────
    idt_init();
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

    // ── Phase 4: display ─────────────────────────────────────────────────────
    tty_init();
    fb_setup(&fb);

    // ── Phase 5: storage ─────────────────────────────────────────────────────
    ata_init();
    klog("ATA initialised\n");
    vfs_mount("/C:", txfs_init(), 0);
    klog("Mounted /C: (TxFS)\n");
    mount_detected_drives();

    // ── Phase 6: networking ──────────────────────────────────────────────────
    pci_init();
    e1000_init();
    net_init();

    // ── Phase 7: launch userspace ────────────────────────────────────────────
    klog("Launching init\n");
    launch_init();
}
