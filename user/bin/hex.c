#include "../tox.h"

static void print_hex_byte(uint8_t b) {
    const char* h = "0123456789ABCDEF";
    char buf[3]; buf[0] = h[b >> 4]; buf[1] = h[b & 0xF]; buf[2] = 0;
    print(buf);
}

static void print_hex32(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    char buf[9]; buf[8] = 0;
    for (int i = 7; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    print(buf);
}

void _start() {
    char args[256];
    get_args(args);

    if (!args[0]) {
        set_color(0x0C); print("usage: hex <file>\n");
        set_color(0x07); tox_exit();
    }

    int size = tox_stat(args);
    if (size < 0) {
        set_color(0x0C); print("hex: not found: "); print(args); print("\n");
        set_color(0x07); tox_exit();
    }

    uint8_t* buf = malloc((uint32_t)size);
    if (!buf) {
        set_color(0x0C); print("hex: out of memory\n");
        set_color(0x07); tox_exit();
    }

    int fd = tox_open(args, 1);
    if (fd < 0) {
        set_color(0x0C); print("hex: cannot open: "); print(args); print("\n");
        free(buf); set_color(0x07); tox_exit();
    }
    tox_read(fd, buf, (uint32_t)size);
    tox_close(fd);

    // Print 16 bytes per line
    for (int row = 0; row < size; row += 16) {
        // Offset
        set_color(0x08);
        print_hex32((uint32_t)row);
        print("  ");

        // Hex bytes
        set_color(0x07);
        for (int col = 0; col < 16; col++) {
            if (row + col < size) {
                print_hex_byte(buf[row + col]);
                print(" ");
            } else {
                print("   ");
            }
            if (col == 7) print(" ");
        }

        // ASCII
        set_color(0x0A);
        print(" |");
        for (int col = 0; col < 16 && row + col < size; col++) {
            uint8_t c = buf[row + col];
            char ch[2] = { (c >= 32 && c < 127) ? (char)c : '.', 0 };
            print(ch);
        }
        print("|\n");
        set_color(0x07);
    }

    free(buf);
    tox_exit();
}
