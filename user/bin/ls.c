#include "../tox.h"
void _start() {
    char args[256]; tox_get_args(args);
    const char* path = args[0] ? args : "/C:";

    // Check the path actually exists before listing
    if (tox_isdir(path) < 0) {
        set_color(0x0C); print("ls: no such directory: "); print(path); print("\n");
        set_color(0x07); tox_exit();
    }

    char entry[256]; uint32_t i=0; int found=0;
    while(tox_readdir(path,entry,i)==0) {
        // Hide dotfiles (.keep, .origin metadata, etc.)
        if(entry[0] == '.') { i++; continue; }
        char full[256]; tox_strcpy(full,path);
        int l=tox_strlen(full); full[l]='/'; tox_strcpy(full+l+1,entry);
        if(tox_isdir(full)==1){set_color(0x09);print(entry);print("/\n");}
        else{file_type_t ft=get_file_type(entry);set_color(type_color(ft));
             print(entry);set_color(0x08);print(type_label(ft));print("\n");}
        set_color(0x07); i++; found=1;
    }
    if(!found){set_color(0x07);print("(empty)\n");}
    tox_exit();
}
