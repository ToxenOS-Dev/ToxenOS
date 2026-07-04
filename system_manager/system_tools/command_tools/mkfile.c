// Milestone 19: mkfile -- create an empty file.
// 32-bit ToxenOS equivalents: mkef, touch.
#include <stdint.h>
#include "tox64.h"
#include "toxpath64.h"
#include "toxcolor64.h"

static int my_strlen(const char* s) { int i=0; while(s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }
static void put_err(const char* a, const char* b) {
    sys_set_color(TC64_RED_BRIGHT); put(a); put(b); put("\n"); sys_set_color(TC64_DEFAULT);
}

void _start(void) {
    char args[256];
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;
    toxpath64_strip_quotes(args);

    if (!args[0]) { put_err("mkfile: missing file name", ""); sys_exit(1); }

    char path[256];
    toxpath64_to_internal(args, path, sizeof(path));

    int64_t r = sys_mkfile(path);
    if (r == SYS64_ERR_PROTECTED) { put_err("mkfile: protected path: ", args); sys_exit(1); }
    if (r < 0) { put_err("mkfile: cannot create: ", args); sys_exit(1); }
    sys_exit(0);
    for (;;) {}
}
