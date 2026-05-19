#include "../tox.h"

// Convert Unix octal digit to TxFS permission bits for owner/group/other
// owner: bits 8-6, group: bits 5-3, other: bits 2-0
static uint32_t octal_to_perm(int owner, int group, int other) {
    uint32_t p = 0;
    if (owner & 4) p |= 0x100; // owner r
    if (owner & 2) p |= 0x080; // owner w
    if (owner & 1) p |= 0x040; // owner x
    if (group & 4) p |= 0x020; // group r
    if (group & 2) p |= 0x010; // group w
    if (group & 1) p |= 0x008; // group x
    if (other & 4) p |= 0x004; // other r
    if (other & 2) p |= 0x002; // other w
    if (other & 1) p |= 0x001; // other x
    return p;
}

static void show_mode(uint32_t m) {
    set_color(0x0B);
    print(m & 0x100 ? "r" : "-"); print(m & 0x080 ? "w" : "-"); print(m & 0x040 ? "x" : "-");
    set_color(0x08);
    print(m & 0x020 ? "r" : "-"); print(m & 0x010 ? "w" : "-"); print(m & 0x008 ? "x" : "-");
    print(m & 0x004 ? "r" : "-"); print(m & 0x002 ? "w" : "-"); print(m & 0x001 ? "x" : "-");
    set_color(0x07);
}

void _start() {
    char args[256]; tox_get_args(args);
    if (!args[0]) {
        print("Usage: chmod <octal> <file>   e.g. chmod 644 file.txt\n");
        print("       chmod <file>           show current permissions\n");
        tox_exit();
    }

    // Split args into two tokens
    char tok1[64], tok2[256];
    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && i < 63) { tok1[j++] = args[i++]; }
    tok1[j] = 0;
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i]) { tok2[j++] = args[i++]; }
    tok2[j] = 0;

    // If only one arg, show permissions for that file
    if (!tok2[0]) {
        int mode = tox_getmode(tok1);
        if (mode < 0) {
            set_color(0x0C); print("chmod: not found: "); print(tok1); print("\n");
            set_color(0x07); tox_exit();
        }
        show_mode((uint32_t)mode);
        print("  "); print(tok1); print("\n");
        tox_exit();
    }

    // Parse octal mode (e.g. "644" → 6,4,4)
    if (tok1[0] < '0' || tok1[0] > '7' || tok1[1] < '0' || tok1[1] > '7' ||
        tok1[2] < '0' || tok1[2] > '7' || tok1[3] != 0) {
        set_color(0x0C); print("chmod: invalid mode (use 3-digit octal e.g. 644)\n");
        set_color(0x07); tox_exit();
    }

    uint32_t perm = octal_to_perm(tok1[0]-'0', tok1[1]-'0', tok1[2]-'0');
    if (tox_chmod(tok2, perm) < 0) {
        set_color(0x0C); print("chmod: failed: "); print(tok2); print("\n");
        set_color(0x07); tox_exit();
    }
    show_mode(perm);
    print("  "); print(tok2); print("\n");
    tox_exit();
}
