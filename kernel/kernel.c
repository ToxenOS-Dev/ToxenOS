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

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;

int cursor_x = 0;
int cursor_y = 0;
int prompt_end_x = 0;

extern uint32_t stack_top;


/* write byte to hardware port */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void update_cursor()
{
    uint16_t pos = cursor_y * VGA_WIDTH + cursor_x;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));

    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void scroll()
{
    for (int y = 0; y < VGA_HEIGHT - 1; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = VGA_MEMORY[(y + 1) * VGA_WIDTH + x];

    for (int x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = ' ' | (0x07 << 8);

    cursor_y = VGA_HEIGHT - 1;
}

void clear_screen()
{
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = ' ' | (0x07 << 8);

    cursor_x = 0;
    cursor_y = 0;

    update_cursor();
}

void print_title()
{
    const char* title = "================ Welcome to ToxenOS ================";
    const char* toxen = "Toxen";

    int len = 0;
    while (title[len] != 0) len++;

    int highlight_start = -1;
    for (int i = 0; title[i] != 0; i++)
    {
        int match = 1;
        for (int j = 0; toxen[j] != 0; j++)
        {
            if (title[i + j] != toxen[j]) { match = 0; break; }
        }
        if (match) { highlight_start = i; break; }
    }

    int start = (VGA_WIDTH - len) / 2;

    for (int i = 0; i < len; i++)
    {
        uint8_t color = 0x07;
        if (highlight_start >= 0 && i >= highlight_start && i < highlight_start + 5)
            color = 0x06;

        VGA_MEMORY[start + i] = title[i] | (color << 8);
    }
}

void put_char(char c)
{
    if (c == '\n')
    {
        cursor_x = 0;
        cursor_y++;
    }
    else
    {
        VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = c | (0x07 << 8);
        cursor_x++;
    }

    if (cursor_x >= VGA_WIDTH)
    {
        cursor_x = 0;
        cursor_y++;
    }

    if (cursor_y >= VGA_HEIGHT)
        scroll();

    update_cursor();
}

void print(const char* str)
{
    for (int i = 0; str[i] != 0; i++)
        put_char(str[i]);
}

void print_prompt()
{
    const char* tox = "Tox";

    for (int i = 0; tox[i] != 0; i++)
    {
        VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = tox[i] | (0x06 << 8);
        cursor_x++;
    }

    VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = '>' | (0x07 << 8);
    cursor_x++;

    prompt_end_x = cursor_x;

    update_cursor();
}

void erase_char()
{
    if (cursor_x > prompt_end_x)
    {
        cursor_x--;
        VGA_MEMORY[cursor_y * VGA_WIDTH + cursor_x] = ' ' | (0x07 << 8);
        update_cursor();
    }
}

void print_hex(uint32_t val)
{
    const char* hex = "0123456789ABCDEF";
    char buf[9];
    buf[8] = 0;
    for (int i = 7; i >= 0; i--)
    {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    print(buf);
}


void kernel_main()
{
    clear_screen();
    print_title();
    mm_init();
    paging_init();
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

    cursor_y = 2;
    cursor_x = 0;
    print_prompt();

    while (1)
    {
            if (keyboard_available())
            {
                char c = keyboard_getchar();

                if (c == '\n')
                {
                    put_char('\n');
                    print_prompt();
                }
                else if (c == 8)
                {
                    erase_char();
                }
                else
                {
                    put_char(c);
                }
            }

            __asm__ volatile("hlt");
    }
}