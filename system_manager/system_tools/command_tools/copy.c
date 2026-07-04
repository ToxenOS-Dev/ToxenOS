#include <stdint.h>
#include "tox64.h"
#include "toxpath64.h"
#include "toxcolor64.h"

static int my_strlen(const char* s) { int i=0; while(s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }
static void put_err(const char* a, const char* b) {
    sys_set_color(TC64_RED_BRIGHT); put(a); put(b); put("\n");
    sys_set_color(TC64_DEFAULT);
}

#define COPY_BUF 4096

static char cbuf[COPY_BUF];

static void split2(const char* args, char* t1, int t1max, char* t2, int t2max) {
    t1[0] = t2[0] = 0;
    while (*args == ' ') args++;
    int i = 0;
    while (*args && *args != ' ' && i < t1max-1) t1[i++] = *args++;
    t1[i] = 0;
    while (*args == ' ') args++;
    i = 0;
    while (*args && *args != ' ' && i < t2max-1) t2[i++] = *args++;
    t2[i] = 0;
}

void _start(void) {
    char args[512];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    char src[256], dst[256];
    split2(args, src, sizeof(src), dst, sizeof(dst));

    if (!src[0] || !dst[0]) {
        put_err("copy: usage: copy <source> <dest>", ""); sys_exit(1);
    }

    char isrc[256], idst[256];
    toxpath64_to_internal(src, isrc, sizeof(isrc));
    toxpath64_to_internal(dst, idst, sizeof(idst));

    // Stat source: must exist and must be a file
    uint64_t fsize = 0;
    int is_dir = 0;
    if (sys_stat(isrc, &fsize, &is_dir) < 0) {
        put_err("copy: source not found: ", src); sys_exit(1);
    }
    if (is_dir) {
        put_err("copy: directory copy not supported: ", src); sys_exit(1);
    }
    if (fsize > COPY_BUF) {
        put_err("copy: file too large to copy (4KB limit this version): ", src); sys_exit(1);
    }

    // Read source content
    int64_t fd = sys_open(isrc);
    if (fd < 0) { put_err("copy: cannot open: ", src); sys_exit(1); }
    int64_t total = 0, got;
    while (total < COPY_BUF) {
        got = sys_read((int)fd, cbuf + total, (uint64_t)(COPY_BUF - total));
        if (got <= 0) break;
        total += got;
    }
    sys_close((int)fd);

    // Create destination (fails if it already exists)
    if (sys_mkfile(idst) < 0) {
        put_err("copy: destination already exists or cannot create: ", dst); sys_exit(1);
    }

    // Write content to destination
    if (total > 0 && sys_write_file(idst, cbuf, (uint64_t)total) < 0) {
        put_err("copy: write failed: ", dst); sys_exit(1);
    }
    sys_exit(0);
    for (;;) {}
}
