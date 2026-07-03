// ToxenOS/user64/shell64.c — Milestone 10: ToxenOS64's first interactive
// shell. Runs as a normal NEX64 user process -- its own pid, its own
// address space, like every other Milestone 7/8 process, not a kernel
// component. Reads keyboard input through the new sys_getch/tox_readline
// path, file contents through Milestone 9's open/read/close/stat
// syscalls, and launches other NEX64/ELF64 programs through
// sys_spawn/sys_wait.
//
// Deliberately tiny: a handful of builtins plus "spawn anything else by
// path." No pipes, redirection, aliases, history, environment variables,
// globbing, scripting, or complex quoting -- see the Milestone 10 plan
// for the full out-of-scope list. user/shell.c (the 32-bit shell) was
// read for design inspiration only (the "first word is the command, the
// rest is one argument string" parsing shape) -- nothing here is a port
// of it; this shell uses the current 64-bit syscall API throughout.
//
// Milestone 11: builtins trimmed to just help/exit -- cat and stat are
// now real standalone commands (shw/stat under CMDTOOLS_PATH below),
// using ToxenOS's own naming instead of Unix's. resolve_and_spawn now
// threads the args string through to sys_spawn.
//
// Milestone 12: the visible ToxenOS path style (C:\System Manager\...)
// is now the normal user-facing path -- see user64/toxpath64.h for the
// translation layer to/from TxFS64's internal slash paths, which remain
// the unchanged canonical/backend form. A command token may now be
// quoted (`"C:\System Manager\...\shw.nex"`) so a space-containing
// visible path survives this file's still-whitespace-based tokenizer as
// one token; the legacy internal slash style keeps working unchanged.
//
// Milestone 16: `help` is promoted to a real standalone command
// (system_manager/system_tools/command_tools/help.c) -- same treatment
// cat/stat got in Milestone 11, no redundant builtin kept once a real
// command exists. Only `exit` and `clear`/`cls` remain real builtins
// now (both inherently non-externalizable).
//
// Milestone 17: real cwd navigation (cd/up builtins, ls cwd default).
// Milestone 18: terminal polish -- shell starts in user home folder,
// prompt shows real visible cwd path, `pwd` builtin, pid/exit output
// hidden behind SHELL64_DEBUG, `cd` error says "folder not found",
// toxpath64_to_visible_dir used for all directory display so cwd
// components are Title-Cased (not lowercase_underscore).
#include <stdint.h>
#include "tox64.h"
#include "toxpath64.h"
#include "toxcolor64.h"

#define LINE_MAX 128
#define PATH_MAX 256  // bumped M18: visible cwd in prompt can be ~80 chars

// The user home/profile folder. Bare `cd` and shell startup land here.
// Internal slash form; each component Title-Cases on display via
// toxpath64_to_visible_dir (e.g. "default" -> "Default").
#define HOME_PATH_INTERNAL "/system_manager/user/profiles/default"

// Real external ToxenOS64 commands live here -- internal/canonical
// form. CMDTOOLS_PATH stays the slash path TxFS64 actually understands.
#define CMDTOOLS_PATH TOXPATH64_CMDTOOLS_INTERNAL

// Shell cwd, always internal slash form ("/system_manager/..." etc.).
// Initialized to empty so _start() can set it to HOME_PATH_INTERNAL
// explicitly, making the startup behavior visible in code rather than
// buried in a compile-time initializer.
static char cwd[PATH_MAX];

// Define to show process lifecycle output (pid/exit codes) after every
// spawned command -- useful for debugging, noisy for normal use.
// #define SHELL64_DEBUG 1

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
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

// Milestone 15: prints `s` in color `c`, then resets to the console's
// default -- the shape every colored segment below uses (prompt
// branding, help headers/command names, error messages), so the color
// never bleeds into whatever's printed next.
static void put_colored(uint8_t c, const char* s) {
    sys_set_color(c);
    put(s);
    sys_set_color(TC64_DEFAULT);
}

// Errors are always bright red, reset to default immediately after --
// matches the convention every command_tools program now follows too.
static void put_err(const char* prefix, const char* arg) {
    sys_set_color(TC64_RED_BRIGHT);
    put(prefix);
    put(arg);
    put("\n");
    sys_set_color(TC64_DEFAULT);
}

__attribute__((unused))
static void put_int(int64_t v) {
    char rev[24];
    int rn = 0;
    int neg = v < 0;
    if (neg) v = -v;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }

    char out[24];
    int n = 0;
    if (neg) out[n++] = '-';
    while (rn > 0) out[n++] = rev[--rn];
    out[n] = 0;
    put(out);
}

// Splits a line in place into (cmd, args): cmd is the first whitespace-
// delimited word, args is everything after it (trimmed of leading
// spaces, not further tokenized) -- the milestone's "pass the rest as a
// single argument string" option, kept deliberately simple.
//
// Milestone 12: cmd may instead be a double-quoted token (the minimal
// quoting needed for a space-containing visible path, e.g.
// `"C:\System Manager\...\shw.nex" /hello.ts`) -- if the line starts
// with '"', cmd runs to the matching closing '"' instead of the next
// space, and the quotes themselves are stripped. args is still just
// "the rest of the line," with no quote-awareness of its own.
static void split_cmd(char* line, char** cmd, char** args) {
    while (*line == ' ') line++;

    if (*line == '"') {
        line++;
        *cmd = line;
        while (*line && *line != '"') line++;
        if (*line == '"') { *line = 0; line++; }
        while (*line == ' ') line++;
        *args = line;
        return;
    }

    *cmd = line;
    while (*line && *line != ' ') line++;
    if (*line) {
        *line = 0;
        line++;
        while (*line == ' ') line++;
    }
    *args = line;
}

// Milestone 18: prints the current working directory in visible ToxenOS
// path style with a trailing backslash (e.g. the home folder appears as
// "C:\System Manager\User\Profiles\Default\"). toxpath64_to_visible_dir
// is used so every component including the last is Title-Cased, not
// left as raw lowercase_underscore.
static void cmd_pwd(void) {
    char vis[PATH_MAX];
    toxpath64_to_visible_dir(cwd, vis, sizeof(vis));
    int len = my_strlen(vis);
    put(vis);
    if (len == 0 || vis[len - 1] != '\\') put("\\");
    put("\n");
}

// `cd` mutates shell64's own cwd directly -- must be a builtin because
// a spawned process can't reach back into its parent's memory. No
// argument goes to HOME_PATH_INTERNAL; otherwise the argument is
// resolved via toxpath64_resolve_cwd (handles quoted/relative/absolute
// visible or internal paths). Unquoted multi-word targets like
// `cd System Manager` already arrive here as one string (split_cmd only
// splits cmd from args once) -- quoting is supported, not required.
static void cmd_cd(const char* args) {
    char disp[PATH_MAX];
    str_copy(disp, args, sizeof(disp));
    toxpath64_strip_quotes(disp);

    char target[PATH_MAX];
    if (!*args) {
        str_copy(target, HOME_PATH_INTERNAL, sizeof(target));
    } else {
        toxpath64_resolve_cwd(cwd, args, target, sizeof(target));
    }

    uint64_t size;
    int is_dir;
    if (sys_stat(target, &size, &is_dir) < 0) {
        put_err("cd: folder not found: ", *args ? disp : "(home)");
        return;
    }
    if (!is_dir) {
        put_err("cd: not a folder: ", *args ? disp : "(home)");
        return;
    }

    str_copy(cwd, target, sizeof(cwd));
}

// Resolves a bare command name to a spawnable path and spawns it (with
// `args` passed through as the single raw argument string -- may be
// empty, never NULL here): an exact path -- internal slash style
// (starts with '/') OR visible ToxenOS style (starts with "C:"/"c:",
// translated via toxpath64_to_internal) -- is used as-is, no fallback,
// forcing the external command regardless of any builtin/name
// collision; anything else is tried, in order, as CMDTOOLS_PATH<name>.nex64
// (real installed commands, preferred), then .elf64, then (unchanged,
// backward-compat with existing root-level test fixtures like
// exec64_test) /<name>.nex64, then .elf64. kernel/exec64.c already
// rejects anything that isn't a real NEX64/ELF64-x86_64 binary, loudly
// and safely, regardless of which path got it there. Returns the
// child's pid, or -1 if nothing could be spawned (already reported to
// the user).
static int64_t resolve_and_spawn(const char* cmd, const char* args) {
    char path[PATH_MAX];
    int64_t pid;

    if (toxpath64_looks_like_path(cmd)) {
        char internal[PATH_MAX];
        if (toxpath64_to_internal(cmd, internal, sizeof(internal)) < 0) {
            put_err("shell64: bad path: ", cmd);
            return -1;
        }
        pid = sys_spawn(internal, args);
        if (pid >= 0) return pid;
        put_err("shell64: cannot run ", cmd);
        return -1;
    }

    str_copy(path, CMDTOOLS_PATH, sizeof(path));
    str_cat(path, cmd, sizeof(path));
    str_cat(path, ".nex64", sizeof(path));
    pid = sys_spawn(path, args);
    if (pid >= 0) return pid;

    str_copy(path, CMDTOOLS_PATH, sizeof(path));
    str_cat(path, cmd, sizeof(path));
    str_cat(path, ".elf64", sizeof(path));
    pid = sys_spawn(path, args);
    if (pid >= 0) return pid;

    str_copy(path, "/", sizeof(path));
    str_cat(path, cmd, sizeof(path));
    str_cat(path, ".nex64", sizeof(path));
    pid = sys_spawn(path, args);
    if (pid >= 0) return pid;

    str_copy(path, "/", sizeof(path));
    str_cat(path, cmd, sizeof(path));
    str_cat(path, ".elf64", sizeof(path));
    pid = sys_spawn(path, args);
    if (pid >= 0) return pid;

    put_err("shell64: command not found: ", cmd);
    return -1;
}

static void run_line(char* line) {
    char* cmd;
    char* args;
    split_cmd(line, &cmd, &args);
    if (!*cmd) return;

    if (str_eq(cmd, "exit")) { sys_exit(0); }
    if (str_eq(cmd, "clear") || str_eq(cmd, "cls")) { sys_clear(); return; }
    if (str_eq(cmd, "cd")) { cmd_cd(args); return; }
    if (str_eq(cmd, "up")) { cmd_cd(".."); return; }
    if (str_eq(cmd, "pwd")) { cmd_pwd(); return; }

    // ls.c is a separate process with no access to shell64's cwd state.
    // For bare `ls`, `ls -a`, or `ls --all` (no explicit path) the shell
    // injects the visible cwd as the path so ls.c knows where to list.
    // `ls -a <path>` and `ls <path>` (user-provided explicit paths) are
    // passed through unchanged -- ls.c handles flag+path parsing itself.
    char ls_cwd_arg[PATH_MAX];
    if (str_eq(cmd, "ls")) {
        int needs_cwd = 0;
        const char* prefix = "";
        if (!*args) {
            needs_cwd = 1;
        } else if (str_eq(args, "-a")) {
            needs_cwd = 1; prefix = "-a ";
        } else if (str_eq(args, "--all")) {
            needs_cwd = 1; prefix = "--all ";
        }
        if (needs_cwd) {
            char vis[PATH_MAX];
            toxpath64_to_visible_dir(cwd, vis, sizeof(vis));
            str_copy(ls_cwd_arg, prefix, sizeof(ls_cwd_arg));
            str_cat(ls_cwd_arg, vis, sizeof(ls_cwd_arg));
            args = ls_cwd_arg;
        }
    }

    // Milestone 17-19: all single-path commands resolve relative paths
    // against cwd before spawning -- child processes have no cwd access.
    // The resolved form is always an internal slash path so the command's
    // own toxpath64_to_internal call passes it through unchanged.
    // `where` is deliberately excluded (its arg is a command NAME, not a
    // path -- resolving it against cwd would break its probe logic).
    char wcmd_args[PATH_MAX];
    if (str_eq(cmd, "shw")   || str_eq(cmd, "stat")  ||
        str_eq(cmd, "nexinfo")|| str_eq(cmd, "mkdir") ||
        str_eq(cmd, "mkfile") || str_eq(cmd, "del")) {
        if (*args && !toxpath64_looks_like_path(args)) {
            char stripped[PATH_MAX];
            str_copy(stripped, args, sizeof(stripped));
            toxpath64_strip_quotes(stripped);
            toxpath64_resolve_cwd(cwd, stripped, wcmd_args, sizeof(wcmd_args));
            args = wcmd_args;
        }
    }
    if (str_eq(cmd, "write") && *args) {
        // Split off first token (path) from rest (content).
        char path_tok[PATH_MAX];
        const char* content = args;
        if (*content == '"') {
            content++;
            int i = 0;
            while (*content && *content != '"' && i < (int)sizeof(path_tok)-1)
                path_tok[i++] = *content++;
            path_tok[i] = 0;
            if (*content == '"') content++;
        } else {
            int i = 0;
            while (*content && *content != ' ' && i < (int)sizeof(path_tok)-1)
                path_tok[i++] = *content++;
            path_tok[i] = 0;
        }
        while (*content == ' ') content++;

        if (!toxpath64_looks_like_path(path_tok)) {
            char resolved[PATH_MAX];
            toxpath64_resolve_cwd(cwd, path_tok, resolved, sizeof(resolved));
            str_copy(wcmd_args, resolved, sizeof(wcmd_args));
        } else {
            char internal[PATH_MAX];
            toxpath64_to_internal(path_tok, internal, sizeof(internal));
            str_copy(wcmd_args, internal, sizeof(wcmd_args));
        }
        if (*content) {
            str_cat(wcmd_args, " ", sizeof(wcmd_args));
            str_cat(wcmd_args, content, sizeof(wcmd_args));
        }
        args = wcmd_args;
    }

    int64_t pid = resolve_and_spawn(cmd, args);
    if (pid < 0) return;  // already reported

    // sys_spawn is fully synchronous -- the child has already run to
    // completion by the time it returns (Milestone 9). The exit code is
    // already known; sys_wait just retrieves the cached result.
#ifdef SHELL64_DEBUG
    int64_t code = sys_wait((uint32_t)pid);
    put("[pid "); put_int(pid); put("] exited with code "); put_int(code); put("\n");
#else
    sys_wait((uint32_t)pid);
#endif
}

void _start(void) {
    // Milestone 18: start in the user's home/profile folder, not the
    // drive root -- a terminal session begins at the user's own space,
    // same as modern shells. `cd` with no args returns here too.
    str_copy(cwd, HOME_PATH_INTERNAL, sizeof(cwd));

    char line[LINE_MAX];
    for (;;) {
        // Milestone 18: prompt now shows the real visible cwd so the
        // user always knows where they are. Format: [T] C:\path >
        // toxpath64_to_visible_dir titles-cases every component (so
        // "system_manager" -> "System Manager" in the prompt, not the
        // raw internal form). "[T]" orange branding, path in default
        // light gray, " >" accent in bright green.
        char vis_cwd[PATH_MAX];
        toxpath64_to_visible_dir(cwd, vis_cwd, sizeof(vis_cwd));
        put_colored(TC64_DEFAULT, "[");
        put_colored(TC64_ORANGE, "T");
        put_colored(TC64_DEFAULT, "] ");
        put_colored(TC64_DEFAULT, vis_cwd);
        put_colored(TC64_GREEN_BRIGHT, " >");
        put_colored(TC64_DEFAULT, " ");
        tox_readline(line, sizeof(line));
        run_line(line);
    }
}
