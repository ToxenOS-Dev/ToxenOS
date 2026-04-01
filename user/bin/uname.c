#include "../tox.h"
void _start() {
    set_color(0x06); print("ToxenOS"); set_color(0x07);
    print(" v0.2 - x86 32-bit custom OS\n");
    print("  Kernel:     ToxenOS monolithic kernel\n");
    print("  Arch:       x86 (i386) protected mode, ring 3 userspace\n");
    print("  Filesystem: TxFS (native), FAT32, ext2 (auto-detected)\n");
    print("  Shell:      ToxenSH with command history and cursor navigation\n");
    tox_exit();
}
