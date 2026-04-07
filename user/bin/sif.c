#include "../tox.h"

static int str_contains(const char* haystack, const char* needle) {
    int nlen = tox_strlen(needle);
    int hlen = tox_strlen(haystack);
    for (int i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (int j = 0; j < nlen; j++) {
            if (haystack[i+j] != needle[j]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

void _start() {
    char args[512];
    tox_get_args(args);

    // Split into pattern and file
    char pattern[128], file[256];
    int i = 0, j = 0;

    while (args[i] && args[i] != ' ') pattern[j++] = args[i++];
    pattern[j] = 0;
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i]) file[j++] = args[i++];
    file[j] = 0;

    if (!pattern[0] || !file[0]) {
        set_color(0x0C); print("usage: sif <pattern> <file>\n");
        set_color(0x07); tox_exit();
    }

    int size = tox_stat(file);
    if (size < 0) {
        set_color(0x0C); print("sif: not found: "); print(file); print("\n");
        set_color(0x07); tox_exit();
    }

    char* buf = (char*)malloc((uint32_t)size + 1);
    if (!buf) {
        set_color(0x0C); print("sif: out of memory\n");
        set_color(0x07); tox_exit();
    }

    int fd = tox_open(file, 1);
    tox_read(fd, (uint8_t*)buf, (uint32_t)size);
    tox_close(fd);
    buf[size] = 0;

    // Search line by line
    int line_num = 1;
    int found = 0;
    char line[512];
    int li = 0;

    for (int k = 0; k <= size; k++) {
        char c = buf[k];
        if (c == '\n' || c == 0) {
            line[li] = 0;
            if (str_contains(line, pattern)) {
                // Print line number
                set_color(0x08);
                char num[12]; int n = line_num, ni = 0;
                if (n == 0) { num[ni++] = '0'; }
                else { char tmp[12]; int ti = 0;
                    while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
                    for (int x = ti-1; x >= 0; x--) num[ni++] = tmp[x]; }
                num[ni] = 0;
                print(num); print(": ");
                set_color(0x07); print(line); print("\n");
                found = 1;
            }
            line_num++;
            li = 0;
        } else if (li < 511) {
            line[li++] = c;
        }
    }

    if (!found) {
        set_color(0x08); print("no matches found\n"); set_color(0x07);
    }

    free(buf);
    tox_exit();
}
