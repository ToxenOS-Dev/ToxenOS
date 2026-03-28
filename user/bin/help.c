#include "../tox.h"
void _start() {
    set_color(0x0E); print("ToxenOS Commands:\n"); set_color(0x07);
    print("  ls              - list files\n");
    print("  cd <dir>        - change directory\n");
    print("  cdb             - go up one directory\n");
    print("  pcd             - print current directory\n");
    print("  shw <file>      - show file contents\n");
    print("  mkef <file>     - create empty file\n");
    print("  mkd <dir>       - make directory\n");
    print("  rm <file>       - delete file\n");
    print("  echo <text>     - print text\n");
    print("  file <name>     - show file type\n");
    print("  uname           - OS info\n");
    print("  clear           - clear screen\n");
    print("  reboot          - restart\n");
    print("  shutdown        - power off\n");
    tox_exit();
}
