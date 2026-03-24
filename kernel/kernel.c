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
    ata_init();
    vfs_mount("/disk", txfs_init(), 0);
    tss_init((uint32_t)&stack_top);
    tty_init();

    fb_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    fbterm_init();
    print_title();
    fbterm_draw_indicator();

    // Shell ELF is embedded in the kernel binary
    uint8_t*  elf_buf  = _binary_build_user_shell_elf_start;
    uint32_t  elf_size = (uint32_t)(_binary_build_user_shell_elf_end
                                   - _binary_build_user_shell_elf_start);

    // Validate ELF magic
    if (*(uint32_t*)elf_buf != 0x464C457F) {
        print("Failed to load shell: bad ELF\n");
        while(1) __asm__ volatile("hlt");
    }

    // Spawn shells for TTY 1-3 as isolated processes
    for (int t = 1; t < 4; t++) {
        int pid = process_create_elf("shell", elf_buf, elf_size);
        if (pid < 0) {
            print("Failed to spawn shell\n");
            continue;
        }
        tty_assign_pid(pid, t);
        fbterm_pid_tty[pid] = t;
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
