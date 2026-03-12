#include <stdint.h>
#include "../include/keyboard.h"
#include "../include/irq.h"
#include "../include/pic.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static char scancode_table[128] =
{
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=',8,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',
};

// small ring buffer
#define BUFFER_SIZE 64
static char buffer[BUFFER_SIZE];
static int buf_head = 0;
static int buf_tail = 0;

static void keyboard_handler()
{
    uint8_t scancode = inb(0x60);

    // ignore key releases (bit 7 set) and extended keys
    if (scancode & 0x80) return;
    if (scancode == 0xE0) return;

    char c = scancode_table[scancode];
    if (c == 0) return;

    // put into ring buffer
    int next = (buf_tail + 1) % BUFFER_SIZE;
    if (next != buf_head)   // drop if buffer full
    {
        buffer[buf_tail] = c;
        buf_tail = next;
    }
}

void keyboard_init()
{
    irq_register(1, keyboard_handler);
}

char keyboard_getchar()
{
    // wait until something is in the buffer
    while (buf_head == buf_tail)
        __asm__ volatile("hlt");  // sleep until next interrupt

    char c = buffer[buf_head];
    buf_head = (buf_head + 1) % BUFFER_SIZE;
    return c;
}

int keyboard_available()
{
    return buf_head != buf_tail;
}