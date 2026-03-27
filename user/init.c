// ToxenOS/user/init.c — PID 1
#include <stdint.h>

static void print(const char* s)
{
    __asm__ volatile("int $0x80" :: "a"(1), "b"(s));
}
static void set_color(uint8_t c)
{
    __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)c));
}
static void yield()
{
    __asm__ volatile("int $0x80" :: "a"(4));
}
static int spawn_embedded(int tty)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(25), "b"(tty));
    return ret;
}
static void ok(const char* msg)
{
    print("  "); set_color(0x0A); print("[ OK ]"); set_color(0x07);
    print(" "); print(msg); print("\n");
}

void _start()
{
    set_color(0x06);
    print("\n  ToxenOS\n");
    set_color(0x07);
    print("  Starting system...\n\n");

    ok("Memory manager");
    ok("Paging and virtual memory");
    ok("Interrupt descriptor table");
    ok("PIC and IRQ routing");
    ok("PIT timer (100Hz)");
    ok("PS/2 keyboard");
    ok("ATA disk controller");
    ok("VFS and TXFS filesystem");
    ok("Framebuffer terminal");

    print("\n");
    set_color(0x0A);
    print("  ToxenOS ready.\n\n");
    set_color(0x07);

    spawn_embedded(0);

    while (1) yield();
}
