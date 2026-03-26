#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    const char* path = args[0] ? args : "/disk";
    char entry[256]; uint32_t i=0; int found=0;
    while (readdir(path, entry, i) == 0) {
        char full[256]; tox_strcpy(full, path);
        int len=tox_strlen(full); full[len]='/'; tox_strcpy(full+len+1, entry);
        if (isdir(full)==1) {
            set_color(0x09); print(entry); print("/\n");
        } else {
            file_type_t ft=get_file_type(entry);
            set_color(type_color(ft)); print(entry);
            set_color(0x08); print(type_label(ft)); print("\n");
        }
        set_color(0x07); i++; found=1;
    }
    if (!found) { set_color(0x07); print("(empty)\n"); }
    exit();
}
