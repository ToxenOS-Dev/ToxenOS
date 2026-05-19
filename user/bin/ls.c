#include "../tox.h"

static void show_perm(uint32_t m) {
    set_color(0x08);
    print(m & 0x100 ? "r" : "-"); print(m & 0x080 ? "w" : "-"); print(m & 0x040 ? "x" : "-");
    print("  ");
    set_color(0x07);
}

void _start() {
    char args[256]; tox_get_args(args);
    const char* path = args[0] ? args : "/C:";

    if (tox_isdir(path) < 0) {
        set_color(0x0C); print("ls: no such directory: "); print(path); print("\n");
        set_color(0x07); tox_exit();
    }

    char entry[256]; uint32_t i=0; int found=0;
    while(tox_readdir(path,entry,i)==0) {
        if(entry[0] == '.') { i++; continue; }  // hide dotfiles

        char full[256]; tox_strcpy(full,path);
        int l=tox_strlen(full); full[l]='/'; tox_strcpy(full+l+1,entry);

        int mode = tox_getmode(full);
        if (mode >= 0) show_perm((uint32_t)mode);
        else { set_color(0x08); print("---  "); set_color(0x07); }

        if(tox_isdir(full)==1) {
            set_color(0x09); print(entry); print("/\n");
        } else {
            file_type_t ft=get_file_type(entry);
            set_color(type_color(ft)); print(entry);
            set_color(0x08); print(type_label(ft)); print("\n");
        }
        set_color(0x07); i++; found=1;
    }
    if(!found){ set_color(0x07); print("(empty)\n"); }
    tox_exit();
}
