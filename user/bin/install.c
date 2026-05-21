// ToxenOS Installer — clones the running system to a target drive
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

void _start() {
    set_color(0x0B); print("\n  ToxenOS Installer\n\n"); set_color(0x07);
    print("  Detects drives and clones the running ToxenOS to the target.\n\n");

    // Detect drives
    print("  Detecting drives...\n\n");
    for (int d = 0; d <= 3; d++) {
        uint32_t sec = tox_disk_sectors((uint8_t)d);
        if (sec > 0) {
            if (d == 0) set_color(0x08); else set_color(0x0A);
            print("  Drive "); print_int(d); print(": ");
            set_color(0x07); print_size(sec);
            if (d == 0) { set_color(0x08); print("  (boot/source — don't install here)"); }
            set_color(0x07); print("\n");
        }
    }
    print("\n");

    // Pick target drive
    char input[8];
    int target = -1;
    while (target < 1 || target > 3) {
        uint32_t sec = 0;
        print("  Install to drive (1-3): ");
        read_line(input, sizeof(input));
        if (input[0] >= '1' && input[0] <= '3') {
            target = input[0] - '0';
            sec = tox_disk_sectors((uint8_t)target);
            if (sec == 0) {
                set_color(0x0C); print("  No drive found at slot ");
                print_int(target); print(".\n"); set_color(0x07);
                target = -1;
            }
        } else {
            set_color(0x0C); print("  Enter 1, 2, or 3.\n"); set_color(0x07);
        }
    }

    uint32_t target_sec = tox_disk_sectors((uint8_t)target);
    set_color(0x0E);
    print("\n  WARNING: Drive "); print_int(target);
    print(" ("); print_size(target_sec); print(") will be ERASED!\n");
    set_color(0x07);
    print("  Continue? ("); set_color(0x0A); print("Y");
    set_color(0x07); print("/"); set_color(0x0C); print("n");
    set_color(0x07); print("): ");

    // Drain keyboard buffer thoroughly before reading confirmation
    for (int _d = 0; _d < 2000; _d++) yield();
    while (tox_keyavail()) tox_getchar();
    for (int _d = 0; _d < 500; _d++) yield();
    while (tox_keyavail()) tox_getchar();

    char c = tox_getchar();
    if (c == 'n' || c == 'N') { print("n\n  Cancelled.\n"); tox_exit(); }
    print("y\n\n");

    // Block-level clone: copies entire TxFS (25600 blocks = 100MB) to target
    set_color(0x0B); print("  Cloning ToxenOS to drive "); print_int(target);
    print("...\n"); set_color(0x07);
    print("  (This copies ~100MB and takes 30-60 seconds)\n\n");

    if (tox_install_drive((uint8_t)target) < 0) {
        set_color(0x0C); print("  Installation failed!\n"); set_color(0x07);
        tox_exit();
    }

    set_color(0x0A);
    print("  Installation complete!\n\n");
    set_color(0x07);
    print("  Next steps:\n");
    print("  1. Power off and remove the USB/install media\n");
    print("  2. Set drive "); print_int(target); print(" as the boot device in BIOS/UEFI\n");
    print("  3. ToxenOS will boot from drive "); print_int(target); print("\n\n");
    print("  Note: The installed system is an exact copy of this running session.\n");
    print("  Your users, files, and settings are all included.\n\n");

    tox_exit();
}
