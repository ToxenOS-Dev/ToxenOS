#include "../tox.h"
void _start() {
    set_color(0x0E); print("ToxenOS Commands:\n\n"); set_color(0x07);

    set_color(0x0B); print("  Files & Directories\n"); set_color(0x07);
    print("    ls [dir]        - list files\n");
    print("    cd <dir>        - change directory\n");
    print("    cdb             - go up one directory\n");
    print("    pcd             - print current directory\n");
    print("    shw <file>      - show file contents\n");
    print("    hex <file>      - hex dump a file\n");
    print("    cp <src> <dst>  - copy a file\n");
    print("    tree [dir]      - show directory tree\n");
    print("    mkef <file>     - create empty file\n");
    print("    mkd <dir>       - make directory\n");
    print("    rm <file>       - delete file\n");
    print("    file <file>     - show file type\n");

    set_color(0x0B); print("\n  Drives\n"); set_color(0x07);
    print("    ls /C:              - TxFS main drive\n");
    print("    ls /D:          - secondary drive (auto-detected)\n");

    set_color(0x0B); print("\n  System\n"); set_color(0x07);
    print("    echo <text>     - print text\n");
    print("    uname           - OS info\n");
    print("    clear           - clear screen\n");
    print("    reboot          - restart\n");
    print("    shutdown        - power off\n");

    tox_exit();
}
