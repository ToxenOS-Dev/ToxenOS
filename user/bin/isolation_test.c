// isolation_test.c — verify kernel/user page table isolation
// Uses SYS_PAGE_FLAGS to walk the page table and confirm that
// kernel pages do NOT have the PAGE_USER bit (bit 2) set.
// If any kernel page is user-accessible, isolation is broken.
#include "../tox.h"

// Flags: bit 0=present, bit 1=writable, bit 2=user
#define PF_PRESENT  1
#define PF_WRITE    2
#define PF_USER     4

static void print_hex32(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    char buf[9]; buf[8] = 0;
    for (int i = 7; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    print("0x"); print(buf);
}

static void check(const char* label, uint32_t addr, int expect_user) {
    uint32_t flags = tox_page_flags(addr);
    int present    = flags & PF_PRESENT;
    int user       = flags & PF_USER;

    print("  "); print(label); print(": ");
    if (!present) {
        set_color(0x08); print("[not mapped]");
    } else if (user && !expect_user) {
        set_color(0x0C); print("[FAIL] user bit set on kernel page!");
    } else if (!user && expect_user) {
        set_color(0x0C); print("[FAIL] no user bit on user page!");
    } else {
        set_color(0x0A); print("[OK]");
    }
    set_color(0x07);
    print("  flags="); print_hex32(flags); print("\n");
}

void _start() {
    set_color(0x0B); print("=== Isolation Test ===\n\n"); set_color(0x07);

    print("Kernel pages (should NOT have user bit):\n");
    check("kernel .text    ", 0xC0200000, 0);
    check("kernel_directory", 0xC0286000, 0);
    check("kernel heap     ", 0xC03FE000, 0);
    check("kstack region   ", 0xC14FE000, 0);

    print("\nUser pages (should have user bit):\n");
    check("init .text      ", 0x10000000, 1);
    check("user stack      ", 0xBFFFF000, 1);

    print("\n");
    set_color(0x0B); print("Done.\n"); set_color(0x07);
    tox_exit();
}
