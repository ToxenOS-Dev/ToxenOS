#include "../tox.h"

static void print_tree(const char* path, int depth) {
    char entry[256];
    uint32_t i = 0;

    while (tox_readdir(path, entry, i) == 0) {
        // Print indent
        for (int d = 0; d < depth; d++) print("  ");
        print("|-- ");

        // Build full path
        char full[512];
        int l = 0;
        for (; path[l]; l++) full[l] = path[l];
        full[l++] = '/';
        for (int k = 0; entry[k]; k++) full[l++] = entry[k];
        full[l] = 0;

        if (tox_isdir(full) == 1) {
            set_color(0x09); print(entry); print("/\n"); set_color(0x07);
            print_tree(full, depth + 1);
        } else {
            set_color(0x07); print(entry); print("\n");
        }
        i++;
    }
}

void _start() {
    char args[256];
    tox_get_args(args);
    const char* path = args[0] ? args : "/C:";

    if (tox_isdir(path) < 0) {
        set_color(0x0C); print("tree: no such directory: "); print(path); print("\n");
        set_color(0x07); tox_exit();
    }

    set_color(0x09); print(path); print("/\n"); set_color(0x07);
    print_tree(path, 0);
    tox_exit();
}
