// ToxenOS/system_manager/system_tools/command_tools/ls.c — Milestone
// 15: ToxenOS64's first directory-listing command.
//
// Milestone 18 polish: dotfiles (entries beginning with '.') are hidden
// from normal ls output -- they are internal/dev placeholders not meant
// for normal users. Use `ls -a` or `ls --all` to show them. The flag
// must appear at the start of the argument string; everything after it
// (optionally after a space) is treated as the path. shell64.c already
// injects the visible cwd before spawning ls for the no-arg and flag-
// only cases, so this file just parses whatever string it receives.
#include <stdint.h>
#include "tox64.h"
#include "toxpath64.h"
#include "toxcolor64.h"

#define BUF_MAX 128

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void str_cat(char* dst, const char* src, int max) {
    int i = my_strlen(dst);
    int j = 0;
    while (src[j] && i < max - 1) dst[i++] = src[j++];
    dst[i] = 0;
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

static int has_suffix(const char* s, const char* suf) {
    int sl = my_strlen(s);
    int fl = my_strlen(suf);
    if (fl > sl) return 0;
    for (int i = 0; i < fl; i++) {
        if (s[sl - fl + i] != suf[i]) return 0;
    }
    return 1;
}

// Joins dir + "/" + name into out, without doubling the slash when dir
// is already the bare root "/".
static void build_entry_path(char* out, int max, const char* dir, const char* name) {
    str_copy(out, dir, max);
    int len = my_strlen(out);
    if (len == 0 || out[len - 1] != '/') str_cat(out, "/", max);
    str_cat(out, name, max);
}

static void put_error(const char* prefix, const char* arg) {
    sys_set_color(TC64_RED_BRIGHT);
    put(prefix);
    put(arg);
    put("\n");
    sys_set_color(TC64_DEFAULT);
}

void _start(void) {
    char args[256];  // wide enough for flag + visible cwd path
    int64_t n = sys_get_args(args, sizeof(args));
    if (n < 0) args[0] = 0;

    // Parse an optional -a / --all flag at the very start. Everything
    // that follows the flag (after any spaces) is the path argument.
    // Normal ls hides names starting with '.' (dotfiles are internal dev
    // placeholders not intended for the normal user-facing interface).
    int show_all = 0;
    const char* p = args;
    if (p[0]=='-' && p[1]=='a' && (p[2]==' '||p[2]==0)) {
        show_all = 1; p += 2; while (*p == ' ') p++;
    } else if (p[0]=='-' && p[1]=='-' && p[2]=='a' && p[3]=='l' && p[4]=='l' && (p[5]==' '||p[5]==0)) {
        show_all = 1; p += 5; while (*p == ' ') p++;
    }

    char path_arg[256];
    str_copy(path_arg, p, sizeof(path_arg));
    toxpath64_strip_quotes(path_arg);

    char path[BUF_MAX];
    const char* display_target = path_arg[0] ? path_arg : "C:\\";
    if (!path_arg[0]) {
        str_copy(path, "/", sizeof(path));
    } else {
        toxpath64_to_internal(path_arg, path, sizeof(path));
    }

    uint64_t dir_size;
    int is_dir;
    if (sys_stat(path, &dir_size, &is_dir) < 0) {
        put_error("ls: cannot access ", display_target);
        sys_exit(1);
    }
    if (!is_dir) {
        put_error("ls: not a directory: ", display_target);
        sys_exit(1);
    }

    uint32_t idx = 0;
    int count = 0;
    char name[256];
    while (sys_readdir(path, name, idx) == 0) {
        idx++;
        if (!show_all && name[0] == '.') continue;  // hide dotfiles

        char full[BUF_MAX];
        build_entry_path(full, sizeof(full), path, name);

        uint64_t entry_size;
        int entry_is_dir;
        if (sys_stat(full, &entry_size, &entry_is_dir) < 0) continue;

        int is_program = has_suffix(name, ".nex64") || has_suffix(name, ".elf64");

        char disp[256];
        str_copy(disp, name, sizeof(disp));
        toxpath64_format_component(disp, !entry_is_dir);

        if (entry_is_dir) {
            sys_set_color(TC64_BLUE_BRIGHT);
            put(disp);
            put("\\");
            sys_set_color(TC64_DEFAULT);
        } else if (is_program) {
            sys_set_color(TC64_GREEN_BRIGHT);
            put(disp);
            sys_set_color(TC64_DEFAULT);
        } else {
            put(disp);
        }
        put("\n");

        count++;
    }

    if (count == 0) put("(empty)\n");

    sys_exit(0);

    for (;;) { }  // unreachable
}
