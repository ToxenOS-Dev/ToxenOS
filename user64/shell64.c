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
#include <stdint.h>
#include "tox64.h"

#define LINE_MAX 128
#define PATH_MAX 128

// Milestone 11: "real" external ToxenOS64 commands live here. No /bin,
// no user64/bin -- mirrors (spelled out in full, this time) the spirit
// of the 32-bit /C:/BSM/SystemT/ convention. No drive letter and no
// spaces yet: this is a documented stand-in for the aspirational
// C:\System Manager\System Tools\Command Tools\ visible path, deferred
// until 64-bit gets a real drive-letter root and split_cmd() below gets
// a quoting-aware parser (it currently splits on the first space, so a
// space-containing path would be silently truncated).
#define CMDTOOLS_PATH "/system_manager/system_tools/command_tools/"

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
static void split_cmd(char* line, char** cmd, char** args) {
    while (*line == ' ') line++;
    *cmd = line;
    while (*line && *line != ' ') line++;
    if (*line) {
        *line = 0;
        line++;
        while (*line == ' ') line++;
    }
    *args = line;
}

static void builtin_help(void) {
    put("ToxenOS64 shell64 -- Milestone 11, early interactive userland\n");
    put("Builtins: help, exit\n");
    put("Commands: shw <file>, stat <file>, where <cmd>, nexinfo <file>\n");
    put("A bare command name is tried as " CMDTOOLS_PATH "<name>.nex64,\n");
    put("then .elf64, then (legacy test binaries) /<name>.nex64/.elf64.\n");
    put("An exact path (starting with /) is always used as-is.\n");
    put("Wrong-architecture or malformed binaries are rejected, not run.\n");
    put("No pipes, redirection, aliases, history, env vars, or scripting yet.\n");
}

// Resolves a bare command name to a spawnable path and spawns it (with
// `args` passed through as the single raw argument string -- may be
// empty, never NULL here): an exact path (starts with '/') is used
// as-is, no fallback, forcing the external command regardless of any
// builtin/name collision; anything else is tried, in order, as
// CMDTOOLS_PATH<name>.nex64 (real installed commands, preferred), then
// .elf64, then (unchanged, backward-compat with existing root-level
// test fixtures like exec64_test) /<name>.nex64, then .elf64.
// kernel/exec64.c already rejects anything that isn't a real
// NEX64/ELF64-x86_64 binary, loudly and safely, regardless of which
// path got it there. Returns the child's pid, or -1 if nothing could be
// spawned (already reported to the user).
static int64_t resolve_and_spawn(const char* cmd, const char* args) {
    char path[PATH_MAX];
    int64_t pid;

    if (cmd[0] == '/') {
        pid = sys_spawn(cmd, args);
        if (pid >= 0) return pid;
        put("shell64: cannot run "); put(cmd); put("\n");
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

    put("shell64: command not found: "); put(cmd); put("\n");
    return -1;
}

static void run_line(char* line) {
    char* cmd;
    char* args;
    split_cmd(line, &cmd, &args);
    if (!*cmd) return;

    if (str_eq(cmd, "help")) { builtin_help(); return; }
    if (str_eq(cmd, "exit")) { sys_exit(0); }

    int64_t pid = resolve_and_spawn(cmd, args);
    if (pid < 0) return;  // already reported

    // sys_spawn is synchronous (the child has already run to completion
    // by the time it returns -- Milestone 9), so this never actually
    // blocks; it just retrieves the already-known result. A faulted
    // child still reports a real pid here (it was created successfully)
    // but sys_wait returns -1 for it -- the shell keeps running either way.
    int64_t code = sys_wait((uint32_t)pid);
    put("[pid "); put_int(pid); put("] exited with code "); put_int(code); put("\n");
}

void _start(void) {
    put("ToxenOS64 shell64 -- type 'help' for what works so far\n");

    char line[LINE_MAX];
    for (;;) {
        put("T64> ");
        tox_readline(line, sizeof(line));
        run_line(line);
    }
}
