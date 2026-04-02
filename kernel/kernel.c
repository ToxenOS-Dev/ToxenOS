//ToxenOS/kernel/kernel.c
#include <stdint.h>
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

uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;
int     cursor_x      = 0;
int     cursor_y      = 0;
int     prompt_end_x  = 0;
uint8_t current_color = 0x07;

extern uint32_t stack_top;
extern uint32_t kernel_directory[1024];


void put_char(char c)     { fbterm_putchar(c); }
void erase_char()         { fbterm_erase(); }
void print(const char* s) { for (int i=0;s[i];i++) fbterm_putchar(s[i]); }
void clear_screen()       { fbterm_clear(); fbterm_draw_indicator(); }
void set_color(uint8_t c) { current_color=c; fbterm_set_color(c); }

void print_hex(uint32_t val) {
    const char* h="0123456789ABCDEF";
    char buf[9]; buf[8]=0;
    for (int i=7;i>=0;i--) { buf[i]=h[val&0xF]; val>>=4; }
    print(buf);
}

static void print_title() {
    const char* title="==== Welcome to ToxenOS ====";
    const char* mark="ToxenOS";
    int len=0; while(title[len]) len++;
    int pad=(fbterm_cols()-len)/2; if(pad<0) pad=0;
    for(int i=0;i<pad;i++) fbterm_putchar(' ');
    int ms=-1;
    for(int i=0;title[i];i++) {
        int m=1; for(int j=0;mark[j];j++) if(title[i+j]!=mark[j]){m=0;break;}
        if(m){ms=i;break;}
    }
    for(int i=0;i<len;i++) {
        fbterm_set_color((ms>=0&&i>=ms&&i<ms+7)?0x06:0x07);
        fbterm_putchar(title[i]);
    }
    fbterm_set_color(0x07);
    fbterm_putchar('\n'); fbterm_putchar('\n');
}

// Map a physical range into the kernel directory (used for framebuffer).
static void map_phys_range(uint32_t phys_start, uint32_t size)
{
    uint32_t start = phys_start & ~0xFFF;
    uint32_t end   = (phys_start + size + 0xFFF) & ~0xFFF;
    for (uint32_t addr = start; addr < end; addr += 0x1000)
        paging_map(kernel_directory, addr, addr, PAGE_PRESENT | PAGE_WRITABLE);
}

extern uint8_t _binary_build_user_shell_elf_start[];
extern uint8_t _binary_build_user_shell_elf_end[];
extern uint8_t _binary_build_user_init_elf_start[];
extern uint8_t _binary_build_user_init_elf_end[];

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    uint32_t* mb    = (uint32_t*)mb_info_addr;
    uint32_t  total = mb[0];
    uint8_t*  tag   = (uint8_t*)(mb_info_addr + 8);
    uint8_t*  end   = (uint8_t*)(mb_info_addr + total);

    uint32_t fb_addr=0, fb_width=0, fb_height=0, fb_pitch=0, fb_bpp=0;

    while (tag < end) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag+4);
        if (type == 8) {
            fb_addr   = *(uint32_t*)(tag+8);
            fb_pitch  = *(uint32_t*)(tag+16);
            fb_width  = *(uint32_t*)(tag+20);
            fb_height = *(uint32_t*)(tag+24);
            fb_bpp    = *(uint8_t*) (tag+28);
            break;
        }
        tag += (size+7)&~7;
    }

    mm_init();
    paging_init();

    // Map framebuffer before using it
    if (fb_addr && fb_width && fb_height)
        map_phys_range(fb_addr, fb_pitch * fb_height);

    idt_init();
    pic_remap();
    timer_init(100);
    __asm__ volatile("sti");
    keyboard_init();
    process_init();
    syscall_init();
    vfs_init();
    vfs_mount("/", tmpfs_init(), 0);
    klog("ToxenOS kernel started\n");

    tss_init((uint32_t)&stack_top);
    tty_init();

    fb_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    fbterm_init();
    print_title();
    fbterm_draw_indicator();

    ata_init();
    klog("ATA disk controller initialized\n");
    vfs_mount("/C:", txfs_init(), 0);
    klog("Mounted /C: (TxFS)\n");

    // Auto-detect filesystems on all drives and assign drive letters D: E: F: G:
    {
        static uint8_t probe_buf[512];
        char drive_letter[4] = "/D:";  // start at D:

        // Probe drives 1-3 (primary slave, secondary master, secondary slave)
        // Drive 0 = primary master = always C: (TxFS, already mounted above)
        for (uint8_t drv = 1; drv <= 3; drv++) {
            fs_driver_t* detected = 0;

            // Check for FAT32/16/12: read LBA 1 (offset by 1 for QEMU quirk)
            // Valid FAT BPB has bytes_per_sector=512 and sectors_per_cluster != 0
            int fat_read_ok = ata_read_drive(drv, 1, probe_buf, 1);

            if (fat_read_ok == 1) {
                uint16_t bps = (uint16_t)(probe_buf[11] | ((uint16_t)probe_buf[12] << 8));
                uint8_t  spc = probe_buf[13];
                uint8_t  nfat = probe_buf[16];
                if (bps == 512 && spc != 0 && (nfat == 1 || nfat == 2)) {
                    detected = fat_init();
                }
            }

            // Check for ext2: superblock at byte offset 1024 (LBA 2 on 512-byte sectors)
            // Magic number 0xEF53 is at offset 56 within the superblock
            // Only check if drive responded to the FAT probe (fat_read_ok != -1)
            // This prevents false positives from CD-ROMs and empty slots
            if (!detected && fat_read_ok != -1) {
                int ext2_read_ok = ata_read_drive(drv, 2, probe_buf, 1);

                if (ext2_read_ok == 1) {
                    uint16_t magic = (uint16_t)(probe_buf[56] | ((uint16_t)probe_buf[57] << 8));
                    if (magic == 0xEF53) {
                        detected = ext2_init();
                    }
                }
            }

            // Check for TxFS: superblock at block 1 (LBA 8 for 4096-byte blocks)
            // Magic 0x54584653 ("TXFS") at offset 0 of superblock
            if (!detected && ata_read_drive(drv, 8, probe_buf, 1) == 1) {
                uint32_t magic = (uint32_t)(probe_buf[0] | ((uint32_t)probe_buf[1]<<8) |
                                 ((uint32_t)probe_buf[2]<<16) | ((uint32_t)probe_buf[3]<<24));
                if (magic == 0x54584653) {
                    detected = txfs_init();
                }
            }

            if (detected) {
                // Pass drive number as device string so driver knows which drive to use
                // Also set the driver mountpoint to match the assigned letter
                // Pass "N:mountpoint" so driver knows both drive number and mountpoint
                char drv_str[8];
                drv_str[0] = (char)('0' + drv);
                drv_str[1] = ':';
                drv_str[2] = drive_letter[0];
                drv_str[3] = drive_letter[1];
                drv_str[4] = drive_letter[2];
                drv_str[5] = 0;
                vfs_mount(drive_letter, detected, drv_str);
                drive_letter[1]++;  // advance: D -> E -> F -> G
            }
        }
    }

    // Init ELF is embedded in the kernel binary — Tinit launches the shell
    uint8_t*  elf_buf  = _binary_build_user_init_elf_start;
    uint32_t  elf_size = (uint32_t)(_binary_build_user_init_elf_end
                                   - _binary_build_user_init_elf_start);

    // Validate ELF magic
    if (*(uint32_t*)elf_buf != 0x464C457F) {
        print("Failed to load init: bad ELF\n");
        while(1) __asm__ volatile("hlt");
    }

    // TTY 0: current context drops to ring 3
    tty_for_pid[0]    = 0;
    fbterm_pid_tty[0] = 0;

    // Get entry point from ELF header
    uint32_t entry = *(uint32_t*)(elf_buf + 24);

    // Map ELF segments for TTY0's context (kernel_directory)
    // since jump_to_ring3 uses the current directory
    uint32_t phoff    = *(uint32_t*)(elf_buf + 28);
    uint16_t phentsize = *(uint16_t*)(elf_buf + 42);
    uint16_t phnum    = *(uint16_t*)(elf_buf + 44);

    for (int i = 0; i < phnum; i++) {
        uint8_t* ph = elf_buf + phoff + i * phentsize;
        uint32_t type   = *(uint32_t*)(ph + 0);
        uint32_t offset = *(uint32_t*)(ph + 4);
        uint32_t vaddr  = *(uint32_t*)(ph + 8);
        uint32_t filesz = *(uint32_t*)(ph + 16);
        uint32_t memsz  = *(uint32_t*)(ph + 20);
        uint32_t flags  = *(uint32_t*)(ph + 24);
        if (type != 1 || memsz == 0) continue;

        uint32_t pf = PAGE_PRESENT | PAGE_USER;
        if (flags & 2) pf |= PAGE_WRITABLE;

        uint32_t page_start = vaddr & ~0xFFF;
        uint32_t page_end   = (vaddr + memsz + 0xFFF) & ~0xFFF;

        for (uint32_t va = page_start; va < page_end; va += PAGE_SIZE) {
            uint32_t phys = paging_alloc_page();
            paging_map(kernel_directory, va, phys, pf);
            uint8_t* dst = (uint8_t*)phys;
            for (int j = 0; j < (int)PAGE_SIZE; j++) dst[j] = 0;
            // Copy file data into this page
            uint32_t page_vstart = va;           // virtual start of this page
            uint32_t seg_vstart  = vaddr;        // virtual start of segment
            uint32_t seg_vend    = vaddr + filesz; // virtual end of file data

            for (uint32_t b = 0; b < PAGE_SIZE; b++) {
                uint32_t cur_va = page_vstart + b;
                if (cur_va < seg_vstart) continue;   // before segment start
                if (cur_va >= seg_vend)  break;       // past file data
                uint32_t file_byte = offset + (cur_va - seg_vstart);
                dst[b] = elf_buf[file_byte];
            }
        }
    }

    // Map user stack for TTY0
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint32_t va   = USER_STACK_TOP - (i+1) * PAGE_SIZE;
        uint32_t phys = paging_alloc_page();
        paging_map(kernel_directory, va, phys,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }

    tss_set_kernel_stack((uint32_t)&stack_top);
    jump_to_ring3((void*)entry, USER_STACK_TOP - 4);
}
