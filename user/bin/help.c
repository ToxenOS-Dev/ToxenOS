#include "../tox.h"
void _start() {
    set_color(0x0E); print("ToxenOS Commands:\n\n"); set_color(0x07);

    set_color(0x0B); print("  Files & Directories\n"); set_color(0x07);
    print("    ls [dir]          - list files\n");
    print("    cd <dir>          - change directory\n");
    print("    cdb               - go up one directory\n");
    print("    pcd               - print current directory\n");
    print("    shw <file>        - show file contents\n");
    print("    hex <file>        - hex dump a file\n");
    print("    cp <src> <dst>    - copy a file\n");
    print("    mv <src> <dst>    - move a file\n");
    print("    rname <old> <new> - rename a file\n");
    print("    tree [dir]        - show directory tree\n");
    print("    find [dir] <pat>  - find files by name\n");
    print("    sif <pat> <file>  - search inside a file\n");
    print("    mkef <file>       - create empty file\n");
    print("    mkd <dir>         - make directory\n");
    print("    rm <file>         - delete file\n");
    print("    file <file>       - show file type\n");

    set_color(0x0B); print("\n  Processes\n"); set_color(0x07);
    print("    proc              - list running processes\n");
    print("    top               - live process viewer (q to quit)\n");
    print("    end <pid>         - terminate a process\n");

    set_color(0x0B); print("\n  Shell\n"); set_color(0x07);
    print("    cmd | cmd         - pipe output to next command\n");
    print("    cmd > file        - redirect output to file\n");
    print("    cmd < file        - read input from file\n");
    print("    Tab               - autocomplete command\n");
    print("    Up/Down           - browse command history\n");
    print("    Ctrl+C            - kill running program\n");

    set_color(0x0B); print("\n  System\n"); set_color(0x07);
    print("    bmsg              - show boot messages\n");
    print("    echo <text>       - print text\n");
    print("    uname             - OS info\n");
    print("    clear             - clear screen\n");
    print("    reboot            - restart\n");
    print("    shutdown          - power off\n");

    set_color(0x0B); print("\n  Drives\n"); set_color(0x07);
    print("    ls /C:            - TxFS main drive\n");
    print("    ls /D:            - secondary drive (auto-detected)\n");

    set_color(0x0B); print("\n  Network\n"); set_color(0x07);
    print("    dns <hostname>       - DNS lookup\n");
    print("    http <ip> [path]     - HTTP GET request\n");

    set_color(0x0B); print("\n  Diagnostics\n"); set_color(0x07);
    print("    sleeptest         - test timer sleep & scheduler\n");
    print("    memtest           - test dynamic memory (sbrk/malloc)\n");
    print("    pipetest          - test kernel pipes & blocking I/O\n");
    print("    nettest           - test UDP networking\n");

    tox_exit();
}
