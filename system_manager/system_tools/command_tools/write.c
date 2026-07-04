// Milestone 19: write -- write small text content to a file.
// Usage: write <file> <text content>
// The file must already exist (use mkfile first). Replaces the entire
// file content. Content is limited to ~4KB this milestone (direct blocks
// only). shell64 pre-resolves relative paths and passes them as the
// first space-separated token in args.
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

    if (!args[0]) { put_err("write: usage: write <file> <text>", ""); sys_exit(1); }

    // First token is the file path (already resolved by shell64 if relative).
    // Rest of the string is the text content. Path may be quoted.
    char path_tok[256];
    const char* p = args;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < (int)sizeof(path_tok)-1) path_tok[i++] = *p++;
        path_tok[i] = 0;
        if (*p == '"') p++;
    } else {
        int i = 0;
        while (*p && *p != ' ' && i < (int)sizeof(path_tok)-1) path_tok[i++] = *p++;
        path_tok[i] = 0;
    }
    while (*p == ' ') p++;

    if (!path_tok[0]) { put_err("write: missing file name", ""); sys_exit(1); }
    if (!*p)          { put_err("write: missing text content", ""); sys_exit(1); }

    char path[256];
    toxpath64_to_internal(path_tok, path, sizeof(path));

    int64_t r = sys_write_file(path, p, (uint64_t)my_strlen(p));
    if (r == SYS64_ERR_PROTECTED) { put_err("write: protected path: ", path_tok); sys_exit(1); }
    if (r < 0) { put_err("write: cannot write: ", path_tok); sys_exit(1); }
    sys_exit(0);
    for (;;) {}
}
