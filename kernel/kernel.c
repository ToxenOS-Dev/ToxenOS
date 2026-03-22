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

void put_char(char c)   { fbterm_putchar(c); }
void erase_char()       { fbterm_erase(); }
void print(const char* s) { for (int i=0;s[i];i++) fbterm_putchar(s[i]); }
void clear_screen()     { fbterm_clear(); fbterm_draw_indicator(); }
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

// Map a range of physical pages into the kernel page directory.
// Used to make the framebuffer accessible before paging_init().
static void map_phys_range(uint32_t phys_start, uint32_t size)
{
    // Round down/up to page boundaries
    uint32_t start = phys_start & ~0xFFF;
    uint32_t end   = (phys_start + size + 0xFFF) & ~0xFFF;

    for (uint32_t addr = start; addr < end; addr += 0x1000)
        paging_map(kernel_directory, addr, addr,
                   PAGE_PRESENT | PAGE_WRITABLE);
}

extern uint8_t _binary_build_user_shell_elf_start[];
extern uint8_t _binary_build_user_shell_elf_end[];

void kernel_main(uint32_t magic, uint32_t mb_info_addr)
{
    uint32_t* mb     = (uint32_t*)mb_info_addr;
    uint32_t  total  = mb[0];
    uint8_t*  tag    = (uint8_t*)(mb_info_addr + 8);
    uint8_t*  end    = (uint8_t*)(mb_info_addr + total);

    uint32_t fb_addr=0, fb_width=0, fb_height=0, fb_pitch=0, fb_bpp=0;

    while (tag < end) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag+4);
        if (type == 8) {
            fb_addr   = *(uint32_t*)(tag+8);   // low 32 bits of u64 addr
            fb_pitch  = *(uint32_t*)(tag+16);
            fb_width  = *(uint32_t*)(tag+20);
            fb_height = *(uint32_t*)(tag+24);
            fb_bpp    = *(uint8_t*) (tag+28);
            break;
        }
        tag += (size+7)&~7;
    }

    // Core init that must happen before paging
    mm_init();
    paging_init();  // maps first 1GB identity

    // Map the framebuffer physical memory so we can write to it
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

    // Now safe to use the framebuffer
    fb_init(fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    fbterm_init();
    print_title();
    fbterm_draw_indicator();

    uint8_t*      elf_buf = _binary_build_user_shell_elf_start;
    elf_header_t* ehdr    = (elf_header_t*)elf_buf;

    if (ehdr->magic == 0x464C457F) {
        for (int i=0;i<ehdr->phnum;i++) {
            elf_phdr_t* phdr=(elf_phdr_t*)(elf_buf+ehdr->phoff+i*ehdr->phentsize);
            if (phdr->type!=1||phdr->memsz==0) continue;
            uint8_t* dst=(uint8_t*)phdr->vaddr;
            uint8_t* src=elf_buf+phdr->offset;
            for (uint32_t j=0;j<phdr->memsz;j++)
                dst[j]=(j<phdr->filesz)?src[j]:0;
        }
        tty_for_pid[0]=0; fbterm_pid_tty[0]=0;
        for (int t=1;t<4;t++) {
            int pid=process_create("shell",(void(*)(void))ehdr->entry);
            tty_assign_pid(pid,t);
            fbterm_pid_tty[pid]=t;
        }
        jump_to_ring3((void*)ehdr->entry, 0x600000);
    }

    print("Failed to load shell\n");
    while(1) __asm__ volatile("hlt");
}
