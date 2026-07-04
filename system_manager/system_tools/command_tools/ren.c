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

// Split args (shell pre-resolved internal paths, space-separated) into two tokens.
// Internal paths use underscores instead of spaces, so simple space split is safe.
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

static int str_ncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

void _start(void) {
    char args[512];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    char src[256], dst[256];
    split2(args, src, sizeof(src), dst, sizeof(dst));

    if (!src[0] || !dst[0]) {
        put_err("ren: usage: ren <old> <new>", ""); sys_exit(1);
    }

    char isrc[256], idst[256];
    toxpath64_to_internal(src, isrc, sizeof(isrc));
    toxpath64_to_internal(dst, idst, sizeof(idst));

    // Cycle guard: dest must not be inside src
    int slen = my_strlen(isrc);
    if (my_strlen(idst) > slen && idst[slen] == '/' &&
        str_ncmp(idst, isrc, slen) == 0) {
        put_err("ren: cannot rename a folder into itself: ", src); sys_exit(1);
    }

    int64_t r = sys_rename(isrc, idst);
    if (r == SYS64_ERR_PROTECTED) { put_err("ren: protected path: ", src); sys_exit(1); }
    if (r == SYS64_ERR_EXISTS)    { put_err("ren: target already exists: ", dst); sys_exit(1); }
    if (r < 0)                    { put_err("ren: cannot rename: ", src); sys_exit(1); }
    sys_exit(0);
    for (;;) {}
}
