// ToxenOS Installer — Linux-style interactive installer
#include "../tox.h"

static void print_size(uint32_t sectors) {
    uint32_t mb = (sectors / 2) / 1024;
    if (mb >= 1024) { print_int((int)(mb/1024)); print(" GB"); }
    else { print_int((int)mb); print(" MB"); }
}

static int read_line(char* buf, int max) {
    int i = 0;
    while (i < max-1) {
        char c = tox_getchar();
        if (c == '\n' || c == '\r') { buf[i]=0; print("\n"); return i; }
        if ((c=='\b'||c==127)&&i>0) { i--; tox_erase(); continue; }
        if (c < 0x20 || c > 0x7E) continue;
        buf[i++]=c; char s[2]={c,0}; print(s);
    }
    buf[i]=0; return i;
}

static void drain_keys(void) {
    tox_sleep(400);               // wait for key-up events to arrive
    while (tox_keyavail()) tox_getchar(); // drain all
    tox_sleep(100);
    while (tox_keyavail()) tox_getchar();
}

static void hr(void) {
    set_color(0x08);
    print("  ────────────────────────────────────────\n");
    set_color(0x07);
}

void _start() {
    tox_clear();
    print("\n\n");
    set_color(0x0B); print("  Welcome to the ToxenOS Installer\n"); set_color(0x07);
    hr();
    print("\n");

    // ── Step 1: Detect disks ──────────────────────────────────────────────────
    set_color(0x08); print("  Available disks:\n\n"); set_color(0x07);
    int drive_count = 0;
    for (int d = 0; d <= 3; d++) {
        uint32_t sec = tox_disk_sectors((uint8_t)d);
        if (sec > 0) {
            set_color(0x08); print("    ["); set_color(0x0B);
            print_int(d); set_color(0x08); print("]  ");
            set_color(0x07); print("Drive "); print_int(d); print("  ");
            set_color(d==0 ? 0x08 : 0x0A);
            print_size(sec);
            if (d == 0) { set_color(0x08); print("  ← source (do not select)"); }
            set_color(0x07); print("\n");
            drive_count++;
        }
    }
    if (drive_count < 2) {
        print("\n");
        set_color(0x0C); print("  No target disk found. Connect a second disk and reboot.\n");
        set_color(0x07); tox_exit();
    }
    print("\n");

    // ── Step 2: Select target ─────────────────────────────────────────────────
    char input[16];
    int target = -1;
    while (target < 1 || target > 3) {
        print("  Select installation target (1-3): ");
        read_line(input, sizeof(input));
        if (input[0] >= '1' && input[0] <= '3') {
            int t = input[0] - '0';
            if (tox_disk_sectors((uint8_t)t) > 0) target = t;
            else { set_color(0x0C); print("  No disk at that slot.\n"); set_color(0x07); }
        } else {
            set_color(0x0C); print("  Enter 1, 2, or 3.\n"); set_color(0x07);
        }
    }

    // ── Step 3: Confirm ───────────────────────────────────────────────────────
    uint32_t tsec = tox_disk_sectors((uint8_t)target);
    print("\n");
    set_color(0x0E);
    print("  WARNING: All data on Drive "); print_int(target);
    print(" ("); print_size(tsec); print(") will be lost!\n\n");
    set_color(0x07);
    print("  Proceed with installation? (Y/n): ");

    // Drain all buffered keys (including key-up events from previous inputs)
    tox_sleep(500);
    while (tox_keyavail()) tox_getchar();

    // Only accept explicit y/Y or n/N — ignore Enter and scan codes
    char c = 0;
    while (c != 'y' && c != 'Y' && c != 'n' && c != 'N') {
        c = tox_getchar();
        // ignore non-printable chars (scan codes, newlines from previous input)
        if (c < 0x20 || c > 0x7E) c = 0;
    }
    if (c == 'n' || c == 'N') {
        char s[2]={c,0}; print(s);
        print("\n\n  Installation cancelled.\n");
        tox_exit();
    }
    print("y\n\n");

    // ── Step 4: Configure hostname ────────────────────────────────────────────
    char hostname[64] = "toxenos";
    print("  Hostname [toxenos]: ");
    tox_sleep(300);
    while (tox_keyavail()) tox_getchar();
    char hbuf[64];
    read_line(hbuf, sizeof(hbuf));
    if (hbuf[0]) tox_strcpy(hostname, hbuf);

    // ── Step 5: Install ───────────────────────────────────────────────────────
    print("\n");
    hr();
    set_color(0x0B); print("  Installing ToxenOS...\n\n"); set_color(0x07);

    // Progress bar
    set_color(0x08); print("  Copying filesystem  [");
    set_color(0x07);

    // The kernel copies 25600 blocks — we show a spinner while it runs
    // Since the syscall is blocking, we just show a message
    set_color(0x08); print("copying..."); set_color(0x07);

    if (tox_install_drive((uint8_t)target) < 0) {
        print("\n");
        set_color(0x0C); print("  Installation failed!\n"); set_color(0x07);
        tox_exit();
    }

    set_color(0x0A); print(" done]\n"); set_color(0x07);

    // Set hostname on the installed system
    // (The installed system is a clone; hostname is stored in /etc/reg)
    // We'd need to write to the cloned drive's /etc/reg — skip for now,
    // user can run 'hostname <name>' after booting the installed system.

    print("\n");
    hr();
    set_color(0x0A); print("\n  Installation complete!\n\n"); set_color(0x07);
    print("  What to do next:\n\n");
    print("  1. Power off the machine\n");
    print("  2. Remove the USB/install media\n");
    print("  3. Set Drive "); print_int(target);
    print(" as the boot device in BIOS/UEFI\n");
    print("  4. Boot ToxenOS\n");
    if (hostname[0] && !( hostname[0]=='t'&&hostname[1]=='o'&&hostname[2]=='x')) {
        print("\n  Remember to set your hostname after booting:\n");
        print("    hostname "); print(hostname); print("\n");
    }
    print("\n");
    tox_exit();
}
