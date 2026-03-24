// ToxenOS hello world — a standalone program that runs via exec/spawn
#include <stdint.h>

void print(const char* msg)
{
    __asm__ volatile("int $0x80" :: "a"(1), "b"(msg));
}

void set_color(uint8_t color)
{
    __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)color));
}

void exit()
{
    __asm__ volatile("int $0x80" :: "a"(0));
}

void _start()
{
    set_color(0x0A);
    print("Hello from a separate program!\n");
    print("This is running as its own process in ToxenOS.\n");
    set_color(0x07);
    exit();
}
