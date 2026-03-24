// ToxenOS/user/init.c — PID 1
#include <stdint.h>

static void print(const char* msg)
{
    __asm__ volatile("int $0x80" :: "a"(1), "b"(msg));
}

static void set_color(uint8_t c)
{
    __asm__ volatile("int $0x80" :: "a"(6), "b"((uint32_t)c));
}

static void yield()
{
    __asm__ volatile("int $0x80" :: "a"(4));
}

static int sys_stat(const char* path)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(17), "b"(path));
    return ret;
}

static int spawn_tty(const char* path, int tty)
{
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(24), "b"(path), "c"(tty));
    return ret;
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

static void fail(const char* msg)
{
    print("  "); set_color(0x0C); print("[FAIL]"); set_color(0x07);
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

    print("\n  Launching terminals...\n\n");

    const char* shell = "/disk/shell.elf";
    int shell_on_disk = (sys_stat(shell) >= 0);

    for (int tty = 1; tty <= 3; tty++) {
        int pid = shell_on_disk
                ? spawn_tty(shell, tty)
                : spawn_embedded(tty);

        if (pid >= 0) ok("TTY terminal");
        else          fail("TTY terminal");
    }

    print("\n");
    set_color(0x0A);
    print("  ToxenOS ready.\n");
    set_color(0x07);
    print("  Press Ctrl+Alt+F2 for a shell.\n\n");

    while (1) yield();
}
