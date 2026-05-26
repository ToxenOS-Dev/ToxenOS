// ToxenOS/user/shell.c
#include "tox.h"

// ── Local aliases for tox.h functions ────────────────────────────────────────
// Keep short names used throughout the rest of this file.
static inline void  erase()                                    { tox_erase(); }
static inline int   sys_open(const char* p, int f)             { return tox_open(p, f); }
static inline int   sys_readdir(const char* p, char* o, int i) { return tox_readdir(p, o, (uint32_t)i); }
static inline int   sys_stat(const char* p)                    { return tox_stat(p); }
static inline int   sys_isdir(const char* p)                   { return tox_isdir(p); }
static inline int   sys_my_tty(void)                           { return tox_my_tty(); }
static inline int   sys_spawn_tty_args(const char* p, int t, const char* a) { return tox_spawn_args(p, t, a); }
static inline void  sys_wait(int pid)                          { tox_wait(pid); }
static inline void  sys_sigint_target(int pid)                 { tox_sigint_target(pid); }
static inline int   sys_pipe(int* r, int* w)                   { return tox_pipe(r, w); }
static inline int   sys_spawn_pipe(const char* p, const char* a, int si, int so) { return tox_spawn_pipe(p, a, si, so); }
static inline int   sys_close(int fd)                          { return tox_close(fd); }
static inline int   key_available(void)                        { return tox_keyavail(); }
static inline char  sys_getchar(void)                          { return tox_getchar(); }

// String helper aliases (shell.c uses short names internally)
static inline int   str_len(const char* s)                     { return tox_strlen(s); }
static inline void  str_copy(char* d, const char* s)           { tox_strcpy(d, s); }
static inline int   str_equal(const char* a, const char* b)    { return !tox_strcmp(a, b); }
static inline int   str_starts(const char* s, const char* p)   { return tox_starts_with(s, p); }
static inline void  str_cat(char* d, const char* s)            { tox_strcat(d, s); }


// Forward declarations
static void run_command(char* input);
static void run_sh_script(const char* path, const char* args_str);
static void sh_expand(const char* src, char* dst, int max);

// Arrow key escape codes (sent by keyboard driver)
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04



#define INPUT_MAX   256
#define HISTORY_MAX 16

static int last_exit = 0;  // last command exit code, exposed as $?

static char history[HISTORY_MAX][INPUT_MAX];
static int  history_count = 0;
static int  history_pos   = -1;

static void history_push(const char* cmd) {
    if (!cmd[0]) return;
    if (history_count > 0) {
        int last = (history_count-1) % HISTORY_MAX;
        if (str_equal(history[last], cmd)) return;
    }
    str_copy(history[history_count % HISTORY_MAX], cmd);
    history_count++;
}
static const char* history_get(int offset) {
    if (offset < 0 || offset >= history_count || offset >= HISTORY_MAX) return 0;
    int idx = ((history_count-1-offset) % HISTORY_MAX + HISTORY_MAX) % HISTORY_MAX;
    return history[idx];
}

// ── State ─────────────────────────────────────────────────────────────────────

static char cwd[256] = "/C:";

// ── Aliases ───────────────────────────────────────────────────────────────────
#define ALIAS_MAX     16
#define ALIAS_KEY_MAX 32
#define ALIAS_VAL_MAX 128
static char alias_keys[ALIAS_MAX][ALIAS_KEY_MAX];
static char alias_vals[ALIAS_MAX][ALIAS_VAL_MAX];
static int  alias_count = 0;

static const char* alias_find(const char* name) {
    for (int i = 0; i < alias_count; i++)
        if (str_equal(alias_keys[i], name)) return alias_vals[i];
    return 0;
}
static void alias_set(const char* name, const char* val) {
    for (int i = 0; i < alias_count; i++) {
        if (str_equal(alias_keys[i], name)) { str_copy(alias_vals[i], val); return; }
    }
    if (alias_count >= ALIAS_MAX) return;
    str_copy(alias_keys[alias_count], name);
    str_copy(alias_vals[alias_count], val);
    alias_count++;
}
static void alias_remove(const char* name) {
    for (int i = 0; i < alias_count; i++) {
        if (str_equal(alias_keys[i], name)) {
            for (int j = i; j < alias_count-1; j++) {
                str_copy(alias_keys[j], alias_keys[j+1]);
                str_copy(alias_vals[j], alias_vals[j+1]);
            }
            alias_count--; return;
        }
    }
}

// ── Alias persistence ──────────────────────────────────────────────────────────
#define ALIAS_FILE    "/C:/etc/aliases"
#define HISTORY_FILE  "/C:/etc/history"

static void alias_save(void) {
    int fd = sys_open(ALIAS_FILE, 0x6);  // WRITE|CREATE|TRUNC
    if (fd < 0) return;
    for (int i = 0; i < alias_count; i++) {
        tox_write(fd, (const uint8_t*)alias_keys[i], (uint32_t)str_len(alias_keys[i]));
        tox_write(fd, (const uint8_t*)"=", 1);
        tox_write(fd, (const uint8_t*)alias_vals[i], (uint32_t)str_len(alias_vals[i]));
        tox_write(fd, (const uint8_t*)"\n", 1);
    }
    sys_close(fd);
}
static void alias_load(void) {
    int sz = sys_stat(ALIAS_FILE);
    if (sz <= 0) return;
    char* buf = malloc((uint32_t)sz+1);
    if (!buf) return;
    int fd = sys_open(ALIAS_FILE, 0x1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)sz);
    sys_close(fd); if(n<0)n=0; buf[n]=0;
    char* p = buf;
    while (*p) {
        char name[ALIAS_KEY_MAX], val[ALIAS_VAL_MAX];
        int ni=0, vi=0;
        while(*p && *p!='=' && *p!='\n' && ni<ALIAS_KEY_MAX-1) name[ni++]=*p++;
        name[ni]=0;
        if (*p=='=') { p++; while(*p && *p!='\n' && vi<ALIAS_VAL_MAX-1) val[vi++]=*p++; }
        val[vi]=0;
        if (*p=='\n') p++;
        if (ni>0) alias_set(name, val);
    }
    free(buf);
}

static void history_append(const char* cmd) {
    if (!cmd[0]) return;
    int fd = sys_open(HISTORY_FILE, 0x2|0x4|0x8);  // WRITE|CREATE|APPEND
    if (fd < 0) return;
    tox_write(fd, (const uint8_t*)cmd, (uint32_t)str_len(cmd));
    tox_write(fd, (const uint8_t*)"\n", 1);
    sys_close(fd);
}
static void history_load(void) {
    int sz = sys_stat(HISTORY_FILE);
    if (sz <= 0) return;
    char* buf = malloc((uint32_t)sz+1);
    if (!buf) return;
    int fd = sys_open(HISTORY_FILE, 0x1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)sz);
    sys_close(fd); if(n<0)n=0; buf[n]=0;
    // Collect up to HISTORY_MAX lines from the file
    static char hlines[HISTORY_MAX][INPUT_MAX];
    int hcount = 0;
    char* p = buf;
    while (*p) {
        char line[INPUT_MAX]; int li=0;
        while(*p && *p!='\n' && li<INPUT_MAX-1) line[li++]=*p++;
        if (*p=='\n') p++;
        line[li]=0;
        if (li > 0) {
            if (hcount < HISTORY_MAX) str_copy(hlines[hcount++], line);
            else {
                for (int i=0;i<HISTORY_MAX-1;i++) str_copy(hlines[i],hlines[i+1]);
                str_copy(hlines[HISTORY_MAX-1], line);
            }
        }
    }
    free(buf);
    for (int i=0;i<hcount;i++) history_push(hlines[i]);
}

// ── Wildcard/glob ─────────────────────────────────────────────────────────────
static int glob_match(const char* pat, const char* name) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            while (*name) { if (glob_match(pat, name)) return 1; name++; }
            return 0;
        }
        if (*pat == '?') { if (!*name) return 0; }
        else if (*pat != *name) return 0;
        pat++; name++;
    }
    return *name == 0;
}

// Expand one wildcard word into out (space-separated matches). Returns match count.
static int glob_expand_word(const char* word, char* out, int maxout) {
    int last_slash = -1;
    for (int i = 0; word[i]; i++) if (word[i] == '/') last_slash = i;

    char dir[256], pattern[128];
    if (last_slash < 0) { str_copy(dir, cwd); str_copy(pattern, word); }
    else {
        int i = 0;
        for (; i < last_slash; i++) dir[i] = word[i]; dir[i] = 0;
        str_copy(pattern, word + last_slash + 1);
    }

    char entry[256]; int idx = 0, matched = 0, oi = 0;
    while (sys_readdir(dir, entry, idx++) == 0) {
        if (entry[0] == '.') continue;
        if (!glob_match(pattern, entry)) continue;
        if (matched > 0 && oi < maxout-1) out[oi++] = ' ';
        char full[256]; str_copy(full, dir);
        int dl = str_len(full);
        if (dl > 0 && full[dl-1] != '/') { full[dl] = '/'; full[dl+1] = 0; }
        str_cat(full, entry);
        for (int i = 0; full[i] && oi < maxout-1; i++) out[oi++] = full[i];
        matched++;
    }
    if (!matched) { str_copy(out, word); return 0; }
    out[oi] = 0;
    return matched;
}

// Expand wildcards in an entire segment string
static char glob_seg[4096];
static void glob_expand_seg(const char* seg) {
    int ei = 0;
    const char* p = seg;
    while (*p) {
        while (*p == ' ' && ei < 2047) glob_seg[ei++] = *p++;
        if (!*p) break;
        const char* ws = p;
        int has_wild = 0;
        while (*p && *p != ' ') { if (*p=='*'||*p=='?') has_wild=1; p++; }
        int wlen = (int)(p - ws);
        if (!has_wild) {
            for (int i = 0; i < wlen && ei < 2047; i++) glob_seg[ei++] = ws[i];
        } else {
            char word[256]; int wl = wlen < 255 ? wlen : 255;
            for (int i = 0; i < wl; i++) word[i] = ws[i]; word[wl] = 0;
            static char expanded[4096];
            glob_expand_word(word, expanded, 4096);
            for (int i = 0; expanded[i] && ei < 2047; i++) glob_seg[ei++] = expanded[i];
        }
    }
    glob_seg[ei] = 0;
}

// ── Background jobs ──────────────────────────────────────────────────────────
#define JOBS_MAX 8
static int job_pids[JOBS_MAX];
static int job_count = 0;

static void shell_print_int(int n) {
    if (n < 0) { print("-"); n = -n; }
    char buf[12]; int i = 10; buf[11] = 0;
    if (!n) { buf[i--] = '0'; }
    while (n) { buf[i--] = '0' + n % 10; n /= 10; }
    print(buf + i + 1);
}
// Print KB value as "X.X MB" (>=1MB) or "X KB"
static void print_size_kb(uint32_t kb) {
    if (kb >= 1024) {
        shell_print_int((int)(kb / 1024));
        print(".");
        shell_print_int((int)((kb % 1024) * 10 / 1024));
        print(" MB");
    } else {
        shell_print_int((int)kb);
        print(" KB");
    }
}

// ── Shell-local environment table ────────────────────────────────────────────
// Stored entirely in user space — no kernel syscalls needed.

#define ENV_MAX      16
#define ENV_KEY_MAX  32
#define ENV_VAL_MAX  128

static char env_keys[ENV_MAX][ENV_KEY_MAX];
static char env_vals[ENV_MAX][ENV_VAL_MAX];
static int  env_count = 0;

static int env_get(const char* name, char* buf, int max) {
    for (int i = 0; i < env_count; i++) {
        if (str_equal(env_keys[i], name)) {
            int j = 0;
            while (env_vals[i][j] && j < max - 1) { buf[j] = env_vals[i][j]; j++; }
            buf[j] = 0;
            return j;
        }
    }
    return -1;
}

static void env_set(const char* name, const char* val) {
    for (int i = 0; i < env_count; i++) {
        if (str_equal(env_keys[i], name)) {
            int j = 0;
            while (val[j] && j < ENV_VAL_MAX - 1) { env_vals[i][j] = val[j]; j++; }
            env_vals[i][j] = 0;
            return;
        }
    }
    if (env_count >= ENV_MAX) return;
    int k = 0;
    while (name[k] && k < ENV_KEY_MAX - 1) { env_keys[env_count][k] = name[k]; k++; }
    env_keys[env_count][k] = 0;
    int v = 0;
    while (val[v] && v < ENV_VAL_MAX - 1) { env_vals[env_count][v] = val[v]; v++; }
    env_vals[env_count][v] = 0;
    env_count++;
}

// ── PATH search ──────────────────────────────────────────────────────────────

static int find_in_path(const char* cmd, char* out) {
    if (cmd[0] == '/') {
        str_copy(out, cmd);
        if (sys_stat(out) >= 0) return 1;
        str_copy(out, cmd); str_cat(out, ".elf");
        if (sys_stat(out) >= 0) return 1;
        return 0;
    }

    // Try kernel global env first, then shell-local, then hardcoded fallback.
    char path_env[256];
    if (tox_getenv("PATH", path_env, sizeof(path_env)) < 0)
        if (env_get("PATH", path_env, sizeof(path_env)) < 0)
            str_copy(path_env, "/C:/BSM/SystemT:/C:/BSM/usr/lst");

    char dir[128];
    const char* p = path_env;
    while (*p) {
        int dlen = 0;
        while (*p && dlen < 127) {
            if (*p == ';') break;
            // ':' is a separator unless it's the drive-letter colon (e.g. /C: or C:)
            if (*p == ':') {
                int is_drive = (dlen == 2 && dir[0] == '/' &&
                               ((dir[1]>='A'&&dir[1]<='Z')||(dir[1]>='a'&&dir[1]<='z'))) ||
                               (dlen == 1 &&
                               ((dir[0]>='A'&&dir[0]<='Z')||(dir[0]>='a'&&dir[0]<='z')));
                if (is_drive) { dir[dlen++] = *p++; continue; }
                break;
            }
            dir[dlen++] = *p++;
        }
        dir[dlen] = 0;
        if (*p == ';' || *p == ':') p++;
        if (!dlen) continue;

        str_copy(out, dir); str_cat(out, "/"); str_cat(out, cmd);
        if (sys_stat(out) >= 0) return 1;
        str_copy(out, dir); str_cat(out, "/"); str_cat(out, cmd); str_cat(out, ".elf");
        if (sys_stat(out) >= 0) return 1;
    }
    return 0;
}

// ── Prompt ───────────────────────────────────────────────────────────────────

static void print_prompt() {
    set_color(0x07); print("[");
    set_color(0x06); print("T");
    set_color(0x07); print("] \\\\");
    char drive[3] = {cwd[1], cwd[2], 0};
    print(drive);
    const char* p = cwd + 3;
    while (*p) {
        if (*p == '/') print("\\");
        else { char buf[2] = {*p, 0}; print(buf); }
        p++;
    }
    set_color(0x0A); print("\\");
    set_color(0x07); print("> ");
}

static void erase_n(int n) { for(int i=0;i<n;i++) erase(); }

// ── Tab completion ────────────────────────────────────────────────────────────

static void do_tab_complete(char* input, int* len_p, int* cur_p) {
    int len = *len_p, cur = *cur_p;

    // Only complete first word (no spaces before cursor)
    for (int i = 0; i < cur; i++) if (input[i] == ' ') return;

    static char partial[64];
    int plen = cur < 63 ? cur : 63;
    for (int i = 0; i < plen; i++) partial[i] = input[i];
    partial[plen] = 0;

    static char match[64];
    match[0] = 0;
    int  match_count = 0;

    static const char tab_dirs[3][16] = {"/C:/Programs","/C:/bin","/D:/Programs"};
    for (int d = 0; d < 3; d++) {
        static char entry[64];
        for (int idx = 0; sys_readdir(tab_dirs[d], entry, idx) >= 0; idx++) {
            int elen = str_len(entry);
            if (elen > 4 && entry[elen-4]=='.' && entry[elen-3]=='e' &&
                            entry[elen-2]=='l' && entry[elen-1]=='f') {
                entry[elen-4] = 0;
                if (str_starts(entry, partial)) { str_copy(match, entry); match_count++; }
            }
        }
    }

    if (match_count == 0) return;

    if (match_count == 1) {
        // Unique match: replace word
        erase_n(cur);
        for (int i = cur; i < len; i++) { char b[2]={input[i],0}; print(b); }
        erase_n(len - cur);

        static char new_input[INPUT_MAX];
        str_copy(new_input, match);
        str_cat(new_input, input + cur);
        str_copy(input, new_input);
        *len_p = str_len(input);
        *cur_p = str_len(match);

        for (int i = 0; i < *len_p; i++) { char b[2]={input[i],0}; print(b); }
        for (int i = *cur_p; i < *len_p; i++) print("\x08");
    } else {
        // Multiple: list them
        print("\n");
        static const char tab_dirs2[3][16] = {"/C:/Programs","/C:/bin","/D:/Programs"};
        for (int d = 0; d < 3; d++) {
            static char entry[64];
            for (int idx = 0; sys_readdir(tab_dirs2[d], entry, idx) >= 0; idx++) {
                int elen = str_len(entry);
                if (elen > 4 && entry[elen-4]=='.' && entry[elen-3]=='e' &&
                                entry[elen-2]=='l' && entry[elen-1]=='f') {
                    entry[elen-4] = 0;
                    if (str_starts(entry, partial)) {
                        set_color(0x0A); print(entry); set_color(0x07); print("  ");
                    }
                }
            }
        }
        print("\n");
        print_prompt();
        input[len] = 0; print(input);
        for (int i = cur; i < len; i++) print("\x08");
    }
}

// ── Builtins ─────────────────────────────────────────────────────────────────

static void cmd_cd(const char* args) {
    if (!args || !args[0]) { print("Usage: cd <dir>\n"); return; }
    char newpath[256];
    if (args[0] == '/') {
        str_copy(newpath, args);
    } else if (args[0]=='.' && args[1]=='.' && (!args[2]||args[2]=='/')) {
        str_copy(newpath, cwd);
        int l = str_len(newpath);
        while (l > 3 && newpath[l-1] != '/') l--;
        if (l > 3) l--;
        newpath[l] = 0;
        if (args[2]=='/') { str_cat(newpath,"/"); str_cat(newpath,args+3); }
    } else {
        str_copy(newpath, cwd);
        int l = str_len(newpath);
        newpath[l] = '/';
        str_copy(newpath+l+1, args);
    }
    if (sys_isdir(newpath) != 1) {
        set_color(0x0C); print("cd: not a directory: "); print(newpath); print("\n");
        set_color(0x07); return;
    }
    str_copy(cwd, newpath);
    tox_setenv("CWD", cwd);
}

// ── Single-command spawner ────────────────────────────────────────────────────

// Returns pid, -2 if not found, -1 on spawn error
static int spawn_cmd(const char* cmd, const char* args, int stdin_fd, int stdout_fd) {
    static char path[128];
    if (!find_in_path(cmd, path)) return -2;

    static char full_args[256];
    // Keyword commands take subcommands/text, not file paths — don't use cwd as default arg
    int is_keyword = str_equal(cmd,"echo")||str_equal(cmd,"tox")||str_equal(cmd,"reg")||
                     str_equal(cmd,"kill")||str_equal(cmd,"hostname")||str_equal(cmd,"adduser")||
                     str_equal(cmd,"passwd")||str_equal(cmd,"usermod")||str_equal(cmd,"sysctl")||
                     str_equal(cmd,"where")||str_equal(cmd,"touch")||str_equal(cmd,"whoami")||str_equal(cmd,"date")||str_equal(cmd,"free")||
                     str_equal(cmd,"df")||str_equal(cmd,"wc")||str_equal(cmd,"syslog")||
                     str_equal(cmd,"trash")||str_equal(cmd,"help")||str_equal(cmd,"uname")||
                     str_equal(cmd,"proc")||str_equal(cmd,"top")||str_equal(cmd,"bmsg")||
                     str_equal(cmd,"ts")||str_equal(cmd,"edit")||str_equal(cmd,"run")||
                     str_equal(cmd,"chmod");
    if (args && args[0]) {
        // Only prepend cwd for args that look like relative file paths
        // Don't prepend for IPs (start with digit), hostnames with dots,
        // or absolute paths (start with /)
        int looks_like_path = 1;
        if (args[0] == '/') looks_like_path = 0;  // already absolute
        if (args[0] == '-') looks_like_path = 0;  // flag/option argument
        if (args[0] >= '0' && args[0] <= '9') looks_like_path = 0;  // IP or number
        // Commands that take subcommands/keywords, not file paths
        if (str_equal(cmd, "echo")) looks_like_path = 0;
        if (str_equal(cmd, "tox"))      looks_like_path = 0;
        if (str_equal(cmd, "reg"))      looks_like_path = 0;
        if (str_equal(cmd, "kill"))     looks_like_path = 0;
        if (str_equal(cmd, "hostname")) looks_like_path = 0;
        if (str_equal(cmd, "adduser"))  looks_like_path = 0;
        if (str_equal(cmd, "passwd"))   looks_like_path = 0;
        if (str_equal(cmd, "usermod"))  looks_like_path = 0;
        if (str_equal(cmd, "help"))     looks_like_path = 0;
        if (str_equal(cmd, "uname"))    looks_like_path = 0;
        if (str_equal(cmd, "proc"))     looks_like_path = 0;
        if (str_equal(cmd, "top"))      looks_like_path = 0;
        if (str_equal(cmd, "bmsg"))     looks_like_path = 0;
        // Hostnames (google.com) have dots but no slash — don't prepend cwd.
        // Exception: .elf files are always local paths, not hostnames.
        int has_dot = 0, has_slash = 0;
        const char* first_space = args;
        while (*first_space && *first_space != ' ') first_space++;
        int tlen = (int)(first_space - args);
        // Find last dot position — extension is chars after it
        int last_dot = -1;
        for (int k = 0; k < tlen; k++) if (args[k] == '.') last_dot = k;
        int ext_len = (last_dot >= 0) ? tlen - last_dot - 1 : 0;
        // Has a file-like extension (1–5 chars): .txt .c .sh .elf .log etc.
        int has_file_ext = (ext_len >= 1 && ext_len <= 5);
        for (const char* q = args; *q && *q != ' '; q++) {
            if (*q == '.') has_dot = 1;
            if (*q == '/') has_slash = 1;
        }
        // Dot with no slash and no recognised extension = likely hostname (google.com)
        if (has_dot && !has_slash && !has_file_ext) looks_like_path = 0;

        if (looks_like_path && args[0] != '/') {
            str_copy(full_args, cwd);
            int l = str_len(full_args);
            full_args[l] = '/';
            str_copy(full_args+l+1, args);
        } else {
            str_copy(full_args, args);
        }
    } else {
        if (is_keyword) full_args[0] = 0;
        else str_copy(full_args, cwd);
    }

    if (stdin_fd == -1 && stdout_fd == -1) {
        int tty = sys_my_tty(); if (tty < 0) tty = 0;
        return sys_spawn_tty_args(path, tty, full_args);
    }
    return sys_spawn_pipe(path, full_args, stdin_fd, stdout_fd);
}

// ── Pipeline executor ─────────────────────────────────────────────────────────

#define MAX_STAGES 4

static void run_pipeline(char* line) {
    // Check for background execution (&) at end of line
    int bg = 0;
    int llen = str_len(line);
    int last = llen - 1;
    while (last >= 0 && line[last] == ' ') last--;
    if (last >= 0 && line[last] == '&') {
        bg = 1;
        line[last] = 0;
        while (last > 0 && line[last-1] == ' ') line[--last] = 0;
    }

    // Split on |
    char* stages[MAX_STAGES];
    int   nstages = 0;
    char* p = line;
    stages[nstages++] = p;
    while (*p && nstages < MAX_STAGES) {
        if (*p == '|') { *p = 0; stages[nstages++] = p+1; }
        p++;
    }

    // Create inter-stage pipes
    int pipe_r[MAX_STAGES], pipe_w[MAX_STAGES];
    // Track redirect fds per stage — close AFTER wait (not before)
    int stage_redir_in[MAX_STAGES], stage_redir_out[MAX_STAGES];
    for (int i = 0; i < MAX_STAGES; i++) stage_redir_in[i] = stage_redir_out[i] = -1;
    for (int i = 0; i < MAX_STAGES; i++) pipe_r[i] = pipe_w[i] = -1;
    for (int i = 0; i+1 < nstages; i++) {
        if (sys_pipe(&pipe_r[i], &pipe_w[i]) < 0) {
            set_color(0x0C); print("pipe: failed\n"); set_color(0x07); return;
        }
    }

    int pids[MAX_STAGES];
    for (int i = 0; i < MAX_STAGES; i++) pids[i] = -1;

    for (int s = 0; s < nstages; s++) {
        char* seg = stages[s];
        while (*seg == ' ') seg++;

        // Expand wildcards before tokenizing
        glob_expand_seg(seg);
        char* gseg = glob_seg;

        // Tokenise the segment
        static char tokens[8][128]; int ntok = 0;
        for (int ti=0;ti<8;ti++) tokens[ti][0]=0;
        char* t = gseg;
        while (*t && ntok < 8) {
            while (*t == ' ') t++;
            if (!*t) break;
            int k = 0;
            while (*t && *t != ' ' && k < 127) tokens[ntok][k++] = *t++;
            tokens[ntok][k] = 0; ntok++;
        }

        static char cmd[64], redir_in[128], redir_out[128];
        cmd[0]=redir_in[0]=redir_out[0]=0;
        int redir_append = 0;

        // Extract cmd and redirects from tokens (limit needed for redirect parsing only)
        for (int ti = 0; ti < ntok; ti++) {
            if (str_equal(tokens[ti],">>") && ti+1<ntok) { redir_append=1; str_copy(redir_out, tokens[++ti]); }
            else if (str_equal(tokens[ti],">") && ti+1<ntok) { redir_append=0; str_copy(redir_out, tokens[++ti]); }
            else if (str_equal(tokens[ti],"<") && ti+1<ntok) { str_copy(redir_in,  tokens[++ti]); }
            else if (!cmd[0]) { str_copy(cmd, tokens[ti]); }
        }
        if (!cmd[0]) continue;

        // Build args from full expanded segment (not limited token array)
        // Skip past the command name in gseg to get all args including wildcard matches
        static char args[2048];
        args[0] = 0;
        {
            const char* p2 = gseg;
            while (*p2 == ' ') p2++;
            // skip command name
            while (*p2 && *p2 != ' ') p2++;
            while (*p2 == ' ') p2++;
            // Strip redirect tokens from end: find >/< and cut there
            int alen = 0;
            while (p2[alen]) alen++;
            // Simple copy — redirects already handled via tokens above
            int ai = 0;
            while (*p2 && ai < 2047) args[ai++] = *p2++;
            args[ai] = 0;
            // Remove any trailing redirect tokens (> file or < file)
            // Find first unquoted > or < and truncate there
            for (int i = 0; i < ai - 1; i++) {
                if ((args[i] == '>' || args[i] == '<') &&
                    (i == 0 || args[i-1] == ' ')) {
                    // trim trailing space before redirect
                    int j = i; while (j > 0 && args[j-1] == ' ') j--;
                    args[j] = 0; break;
                }
            }
        }

        int stdin_fd  = (s > 0)         ? pipe_r[s-1] : -1;
        int stdout_fd = (s+1 < nstages) ? pipe_w[s]   : -1;

        // Resolve redirect paths relative to cwd if not absolute
        static char redir_in_full[256], redir_out_full[256];
        if (redir_in[0] && redir_in[0] != '/') {
            str_copy(redir_in_full, cwd); str_cat(redir_in_full, "/"); str_cat(redir_in_full, redir_in);
        } else { str_copy(redir_in_full, redir_in); }
        if (redir_out[0] && redir_out[0] != '/') {
            str_copy(redir_out_full, cwd); str_cat(redir_out_full, "/"); str_cat(redir_out_full, redir_out);
        } else { str_copy(redir_out_full, redir_out); }

        int redir_in_fd = -1, redir_out_fd = -1;
        if (redir_in[0]) {
            redir_in_fd = sys_open(redir_in_full, 0x1);
            if (redir_in_fd < 0) {
                set_color(0x0C); print("cannot open: "); print(redir_in_full); print("\n");
                set_color(0x07); goto cleanup;
            }
            stdin_fd = redir_in_fd;
        }
        if (redir_out[0]) {
            int out_flags = redir_append ? (0x2 | 0x4 | 0x8) : 0x6;
            redir_out_fd = sys_open(redir_out_full, out_flags);
            if (redir_out_fd < 0) {
                set_color(0x0C); print("cannot open: "); print(redir_out_full); print("\n");
                set_color(0x07);
                if (redir_in_fd >= 0) sys_close(redir_in_fd);
                goto cleanup;
            }
            stdout_fd = redir_out_fd;
        }

        int pid = spawn_cmd(cmd, args[0]?args:0, stdin_fd, stdout_fd);

        // Close pipe ends immediately (signals EOF to next stage)
        if (s > 0 && pipe_r[s-1] >= 0)      { sys_close(pipe_r[s-1]); pipe_r[s-1]=-1; }
        if (s+1<nstages && pipe_w[s] >= 0)   { sys_close(pipe_w[s]);   pipe_w[s]=-1;   }
        // Save redirect fds — close AFTER waiting (child needs them open while running)
        stage_redir_in[s]  = redir_in_fd;
        stage_redir_out[s] = redir_out_fd;

        if (pid == -2) {
            set_color(0x0C); print("Unknown command: "); print(cmd); print("\n");
            set_color(0x07); goto cleanup;
        }
        if (pid < 0) {
            set_color(0x0C); print("Failed to run '"); print(cmd);
            print("' (out of memory or process slots)\n");
            set_color(0x07); goto cleanup;
        }
        pids[s] = pid;
    }

    if (bg) {
        int last_pid = pids[nstages-1];
        if (last_pid >= 0 && job_count < JOBS_MAX)
            job_pids[job_count++] = last_pid;
        set_color(0x08); print("[bg] pid "); shell_print_int(last_pid); print("\n"); set_color(0x07);
        goto cleanup;
    }

    if (pids[nstages-1] >= 0) sys_sigint_target(pids[nstages-1]);
    for (int s = 0; s < nstages; s++) {
        if (pids[s] >= 0) {
            int ec = tox_wait_status(pids[s]);
            if (s == nstages-1) last_exit = ec;
        }
    }
    sys_sigint_target(-1);

    // Close redirect fds after children finish (not before — they need them open)
    for (int s = 0; s < MAX_STAGES; s++) {
        if (stage_redir_in[s]  >= 0) sys_close(stage_redir_in[s]);
        if (stage_redir_out[s] >= 0) sys_close(stage_redir_out[s]);
    }

cleanup:
    for (int i = 0; i < MAX_STAGES; i++) {
        if (pipe_r[i] >= 0) sys_close(pipe_r[i]);
        if (pipe_w[i] >= 0) sys_close(pipe_w[i]);
    }
}

// ── Command router ────────────────────────────────────────────────────────────

static void run_command(char* input) {
    while (*input == ' ') input++;
    if (!*input) return;

    char cmd_buf[64]; int ci = 0;
    while (input[ci] && input[ci] != ' ' && ci < 63) { cmd_buf[ci]=input[ci]; ci++; }
    cmd_buf[ci] = 0;
    const char* args = input[ci] ? input+ci+1 : "";

    // Alias substitution — expand alias then re-run
    {
        const char* av = alias_find(cmd_buf);
        if (av) {
            static char alias_line[512];
            str_copy(alias_line, av);
            if (args && args[0]) { str_cat(alias_line, " "); str_cat(alias_line, args); }
            run_command(alias_line);
            return;
        }
    }

    // .ts script: spawn ts.elf <path>
    {
        int clen = str_len(cmd_buf);
        if (clen > 3 && cmd_buf[clen-3] == '.' && cmd_buf[clen-2] == 't' && cmd_buf[clen-1] == 's') {
            static char ts_path[256], ts_args[256];
            if (cmd_buf[0] == '/') str_copy(ts_path, cmd_buf);
            else { str_copy(ts_path, cwd); str_cat(ts_path, "/"); str_cat(ts_path, cmd_buf); }
            str_copy(ts_args, ts_path);
            if (args && args[0]) { str_cat(ts_args, " "); str_cat(ts_args, args); }
            int pid = spawn_cmd("ts", ts_args, -1, -1);
            if (pid >= 0) sys_wait(pid);
            return;
        }
        // .sh script: run inline
        if (clen > 3 && cmd_buf[clen-3] == '.' && cmd_buf[clen-2] == 's' && cmd_buf[clen-1] == 'h') {
            static char sh_path[256];
            if (cmd_buf[0] == '/') str_copy(sh_path, cmd_buf);
            else { str_copy(sh_path, cwd); str_cat(sh_path, "/"); str_cat(sh_path, cmd_buf); }
            run_sh_script(sh_path, args);
            return;
        }
    }

    if (str_equal(cmd_buf,"run")) {
        if (!args || !args[0]) { print("Usage: run <file.ts>\n"); return; }
        static char run_file[256], run_path[256];
        int ri = 0;
        while (args[ri] && args[ri] != ' ' && ri < 255) { run_file[ri] = args[ri]; ri++; }
        run_file[ri] = 0;
        const char* run_rest = args + ri; while (*run_rest == ' ') run_rest++;
        if (run_file[0] == '/') str_copy(run_path, run_file);
        else { str_copy(run_path, cwd); str_cat(run_path, "/"); str_cat(run_path, run_file); }
        int sres = sys_stat(run_path);
        if (sres < 0) {
            set_color(0x0C); print("run: not found: "); print(run_path); print("\n");
            set_color(0x07); return;
        }
        int flen = str_len(run_file);
        int is_ts = flen > 3 && run_file[flen-3]=='.' && run_file[flen-2]=='t' && run_file[flen-1]=='s';
        int is_sh = flen > 3 && run_file[flen-3]=='.' && run_file[flen-2]=='s' && run_file[flen-1]=='h';
        if (is_sh) { run_sh_script(run_path, run_rest); return; }
        int pid;
        if (is_ts) {
            int tty = sys_my_tty(); if (tty < 0) tty = 0;
            pid = sys_spawn_tty_args("/C:/BSM/SystemT/ts.elf", tty, run_path);
        } else {
            pid = spawn_cmd(run_path, run_rest, -1, -1);
        }
        if (pid < 0) {
            set_color(0x0C); print("run: failed to start: "); print(run_path); print("\n");
            set_color(0x07); return;
        }
        sys_wait(pid);
        return;
    }
    if (str_equal(cmd_buf,"cd"))       { cmd_cd(args); return; }
    if (str_equal(cmd_buf,"cdb"))      { cmd_cd(".."); return; }
    if (str_equal(cmd_buf,"clear"))    { tox_clear(); return; }
    if (str_equal(cmd_buf,"reboot"))   { tox_reboot();   return; }
    if (str_equal(cmd_buf,"shutdown")) { tox_shutdown(); return; }
    if (str_equal(cmd_buf,"logout") || str_equal(cmd_buf,"exit")) {
        tox_exit(); return;
    }

    if (str_equal(cmd_buf,"alias")) {
        if (!args || !args[0]) {
            for (int i = 0; i < alias_count; i++) {
                set_color(0x0B); print(alias_keys[i]); set_color(0x08); print("=");
                set_color(0x07); print(alias_vals[i]); print("\n");
            }
            if (!alias_count) { set_color(0x08); print("no aliases\n"); set_color(0x07); }
            return;
        }
        // Parse name=value
        char name[ALIAS_KEY_MAX]; int ni = 0;
        while (args[ni] && args[ni] != '=' && ni < ALIAS_KEY_MAX-1) { name[ni]=args[ni]; ni++; }
        name[ni] = 0;
        const char* val = (args[ni] == '=') ? args+ni+1 : "";
        alias_set(name, val);
        alias_save();
        return;
    }
    if (str_equal(cmd_buf,"unalias")) {
        if (args && args[0]) { alias_remove(args); alias_save(); }
        return;
    }
    if (str_equal(cmd_buf,"source") || (cmd_buf[0]=='.' && !cmd_buf[1])) {
        if (!args || !args[0]) { print("Usage: source <script.sh>\n"); return; }
        // Resolve relative path against cwd (built-ins don't go through spawn_cmd)
        static char src_path[256];
        if (args[0] == '/') { str_copy(src_path, args); }
        else { str_copy(src_path, cwd); str_cat(src_path, "/"); str_cat(src_path, args); }
        int size = sys_stat(src_path);
        if (size < 0) { set_color(0x0C); print("source: not found: "); print(src_path); print("\n"); set_color(0x07); return; }
        // Read file and execute line by line
        int fd = sys_open(src_path, 1);
        if (fd < 0) { set_color(0x0C); print("source: cannot open: "); print(src_path); print("\n"); set_color(0x07); return; }
        static char script_buf[4096];
        int n = tox_read(fd, (uint8_t*)script_buf, 4095);
        tox_close(fd);
        if (n < 0) n = 0;
        script_buf[n] = 0;
        // Execute line by line
        char line[256]; int li = 0;
        for (int i = 0; i <= n; i++) {
            char c = script_buf[i];
            if (c == '\n' || c == '\r' || c == 0) {
                line[li] = 0;
                if (li > 0 && line[0] != '#') run_command(line);
                li = 0;
            } else if (li < 255) {
                line[li++] = c;
            }
        }
        return;
    }
    if (str_equal(cmd_buf,"sysctl")) {
        if (args && args[0]) {
            set_color(0x0C); print("sysctl: no arguments — just run: sysctl\n");
            set_color(0x07); return;
        }
        char buf[128];
        const char* keys[] = {"version","hostname","uptime","procs",0};
        for (int i = 0; keys[i]; i++) {
            if (tox_sysctl(keys[i], buf, sizeof(buf)) < 0) continue;
            set_color(0x0B); print(keys[i]); set_color(0x08); print(" = ");
            set_color(0x07); print(buf); print("\n");
        }
        return;
    }
    if (str_equal(cmd_buf,"jobs")) {
        int found = 0;
        for (int i = 0; i < job_count; i++) {
            if (tox_is_alive(job_pids[i])) {
                set_color(0x0B); print("["); shell_print_int(i+1); print("] ");
                set_color(0x07); print("running  pid "); shell_print_int(job_pids[i]); print("\n");
                found = 1;
            }
        }
        if (!found) { set_color(0x08); print("no background jobs\n"); set_color(0x07); }
        return;
    }
    if (str_equal(cmd_buf,"export")) {
        if (!args || !args[0]) return;
        char name[ENV_KEY_MAX]; int ni = 0;
        while (args[ni] && args[ni] != '=' && ni < ENV_KEY_MAX - 1) { name[ni] = args[ni]; ni++; }
        name[ni] = 0;
        const char* val = (args[ni] == '=') ? args + ni + 1 : "";
        env_set(name, val);
        tox_setenv(name, val);
        return;
    }
    if (str_equal(cmd_buf,"env")) {
        char buf[ENV_VAL_MAX];
        for (int i = 0; i < env_count; i++) {
            print(env_keys[i]); print("="); print(env_vals[i]); print("\n");
        }
        (void)buf;
        return;
    }

    if (str_equal(cmd_buf,"free")) {
        char _b[32];
        uint32_t total=0, free_kb=0;
        if (tox_sysctl("mem.total",_b,sizeof(_b))>=0){uint32_t v=0;for(int i=0;_b[i]>='0'&&_b[i]<='9';i++)v=v*10+(uint32_t)(_b[i]-'0');total=v;}
        if (tox_sysctl("mem.free", _b,sizeof(_b))>=0){uint32_t v=0;for(int i=0;_b[i]>='0'&&_b[i]<='9';i++)v=v*10+(uint32_t)(_b[i]-'0');free_kb=v;}
        uint32_t used = total - free_kb;
        set_color(0x0B); print("         total        used        free\n"); set_color(0x07);
        print("Mem:     ");
        set_color(0x0F); print_size_kb(total);  print("      ");
        set_color(0x0C); print_size_kb(used);   print("      ");
        set_color(0x0A); print_size_kb(free_kb); print("\n"); set_color(0x07);
        return;
    }
    if (str_equal(cmd_buf,"df")) {
        char _b[32];
        uint32_t total=0, free_kb=0;
        if (tox_sysctl("disk.total",_b,sizeof(_b))>=0){uint32_t v=0;for(int i=0;_b[i]>='0'&&_b[i]<='9';i++)v=v*10+(uint32_t)(_b[i]-'0');total=v;}
        if (tox_sysctl("disk.free", _b,sizeof(_b))>=0){uint32_t v=0;for(int i=0;_b[i]>='0'&&_b[i]<='9';i++)v=v*10+(uint32_t)(_b[i]-'0');free_kb=v;}
        uint32_t used = total - free_kb;
        uint32_t pct  = total ? (used * 100 / total) : 0;
        set_color(0x0B); print("Filesystem    Size       Used       Avail    Use%\n"); set_color(0x07);
        print("TxFS        ");
        set_color(0x0F); print_size_kb(total);   print("     ");
        set_color(0x0C); print_size_kb(used);    print("     ");
        set_color(0x0A); print_size_kb(free_kb); print("     "); set_color(0x07);
        shell_print_int((int)pct); print("%\n");
        return;
    }
    if (str_equal(cmd_buf,"chmod")) {
        if (!args || !args[0]) {
            set_color(0x0C); print("Usage: chmod <mode> <path>\n"); set_color(0x07); return;
        }
        // Parse octal mode (e.g. 755, 644)
        uint32_t mode = 0;
        int ai = 0;
        while (args[ai] >= '0' && args[ai] <= '7') { mode = mode * 8 + (uint32_t)(args[ai++] - '0'); }
        while (args[ai] == ' ') ai++;
        const char* cpath = args + ai;
        if (!cpath[0]) { set_color(0x0C); print("chmod: missing path\n"); set_color(0x07); return; }
        static char chmod_path[256];
        if (cpath[0] == '/') str_copy(chmod_path, cpath);
        else { str_copy(chmod_path, cwd); str_cat(chmod_path, "/"); str_cat(chmod_path, cpath); }
        if (tox_chmod(chmod_path, mode) < 0) {
            set_color(0x0C); print("chmod: failed: "); print(chmod_path); print("\n"); set_color(0x07);
        }
        return;
    }

    if (str_equal(cmd_buf,"whoami")) {
        char wbuf[64];
        if (tox_whoami(wbuf, sizeof(wbuf)) > 0) { print(wbuf); print("\n"); }
        else { print("root\n"); }
        return;
    }
    if (str_equal(cmd_buf,"touch")) {
        if (!args || !args[0]) { set_color(0x0C); print("Usage: touch <path>\n"); set_color(0x07); return; }
        static char touch_path[256];
        if (args[0] == '/') str_copy(touch_path, args);
        else { str_copy(touch_path, cwd); str_cat(touch_path, "/"); str_cat(touch_path, args); }
        int tfd = tox_open(touch_path, 0x2 | 0x4);  // WRITE | CREATE
        if (tfd < 0) { set_color(0x0C); print("touch: failed: "); print(touch_path); print("\n"); set_color(0x07); }
        else tox_close(tfd);
        return;
    }
    if (str_equal(cmd_buf,"where")) {
        if (!args || !args[0]) { set_color(0x0C); print("Usage: where <command>\n"); set_color(0x07); return; }
        char where_out[256];
        if (find_in_path(args, where_out)) { print(where_out); print("\n"); }
        else { set_color(0x0C); print("where: not found: "); print(args); print("\n"); set_color(0x07); }
        return;
    }

    run_pipeline(input);
}

// ── Entry point ───────────────────────────────────────────────────────────────

// ── Shell scripting ───────────────────────────────────────────────────────────

#define SH_MAX_LINES  256
#define SH_LINE_MAX   256
#define SH_VAR_MAX    32
#define SH_LOOP_STACK 16
#define SH_POS_MAX    10   // $0-$9

// Positional parameters: sh_pos[0]=$0 (script name), sh_pos[1]=$1, ...
static char sh_pos[SH_POS_MAX][128];
static int  sh_argc = 0;

// Set by 'exit N' inside a script — propagates up through sh_run calls
static int sh_exit_requested = 0;
static int sh_exit_code = 0;

// Lines are heap-allocated inside run_sh_script; these pointers share that buffer.
static char* sh_lines[SH_MAX_LINES];
static int   sh_nlines;

// Trim leading/trailing spaces in place; returns pointer past leading spaces.
static char* sh_trim(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    int l = str_len(s);
    while (l > 0 && (s[l-1]==' '||s[l-1]=='\t')) { s[--l]=0; }
    return s;
}

// Check if line starts with keyword (word boundary).
static int sh_kw(const char* line, const char* kw) {
    int kl = str_len(kw);
    if (str_len(line) < kl) return 0;
    for (int i=0;i<kl;i++) if(line[i]!=kw[i]) return 0;
    char after = line[kl];
    return after==0||after==' '||after=='\t'||after==';';
}

// Expand $VAR, $?, $0-$9, $#, $@, $* references in src → dst.
static void sh_expand(const char* src, char* dst, int max) {
    int di=0;
    for (int i=0; src[i] && di<max-1; ) {
        if (src[i]=='$') {
            if (src[i+1]=='?') {
                i+=2;
                int v = last_exit;
                char tmp[12]; int ti=0;
                if (v==0) { if(di<max-1) dst[di++]='0'; }
                else {
                    if (v<0) { if(di<max-1) dst[di++]='-'; v=-v; }
                    while (v>0&&ti<11) { tmp[ti++]='0'+(v%10); v/=10; }
                    while (ti>0&&di<max-1) dst[di++]=tmp[--ti];
                }
            } else if (src[i+1]>='0' && src[i+1]<='9') {
                int n = src[i+1]-'0'; i+=2;
                const char* pv = (n <= sh_argc) ? sh_pos[n] : "";
                for (int j=0;pv[j]&&di<max-1;j++) dst[di++]=pv[j];
            } else if (src[i+1]=='#') {
                i+=2;
                int v=sh_argc;
                if(v==0){if(di<max-1)dst[di++]='0';}
                else{char tmp[12];int ti=0;while(v>0&&ti<11){tmp[ti++]='0'+(v%10);v/=10;}while(ti>0&&di<max-1)dst[di++]=tmp[--ti];}
            } else if (src[i+1]=='@' || src[i+1]=='*') {
                i+=2;
                for(int a=1;a<=sh_argc;a++){
                    if(a>1&&di<max-1) dst[di++]=' ';
                    for(int j=0;sh_pos[a][j]&&di<max-1;j++) dst[di++]=sh_pos[a][j];
                }
            } else if (src[i+1]=='_'||(src[i+1]>='a'&&src[i+1]<='z')||(src[i+1]>='A'&&src[i+1]<='Z')) {
                i++; char vname[64]; int vi=0;
                while (src[i] && (src[i]=='_'||(src[i]>='a'&&src[i]<='z')||(src[i]>='A'&&src[i]<='Z')||(src[i]>='0'&&src[i]<='9')) && vi<63)
                    vname[vi++]=src[i++];
                vname[vi]=0;
                char vval[256]; vval[0]=0;
                tox_getenv(vname, vval, sizeof(vval));
                for (int j=0;vval[j]&&di<max-1;j++) dst[di++]=vval[j];
            } else {
                dst[di++]=src[i++];
            }
        } else {
            dst[di++]=src[i++];
        }
    }
    dst[di]=0;
}

// Run one expanded line as a shell command; return exit code (0=ok).
static int sh_eval(char* line) {
    char exp[SH_LINE_MAX]; sh_expand(line, exp, SH_LINE_MAX);
    char* l = sh_trim(exp);
    if (!l[0] || l[0]=='#') return 0;
    // VAR=value assignment?
    {
        int ei=-1;
        for (int i=0;l[i]&&l[i]!=' ';i++) if(l[i]=='='){ei=i;break;}
        if (ei>0) {
            char vn[64]; int vi=0;
            int valid=1;
            for (int i=0;i<ei;i++){
                if(!((l[i]>='a'&&l[i]<='z')||(l[i]>='A'&&l[i]<='Z')||(l[i]>='0'&&l[i]<='9')||l[i]=='_'))
                    {valid=0;break;}
                vn[vi++]=l[i];
            }
            vn[vi]=0;
            if (valid && vi>0) {
                tox_setenv(vn, l+ei+1);
                return 0;
            }
        }
    }
    // Run as shell command
    run_command(l);
    return 0;
}

// Run one line as condition command; return 0=true, nonzero=false.
static int sh_eval_cond(char* line) {
    char exp[SH_LINE_MAX]; sh_expand(line, exp, SH_LINE_MAX);
    char* l = sh_trim(exp);
    if (!l[0]) return 1;

    // Built-in test: [ ... ] or test ...
    int is_bracket = (l[0]=='['&&l[str_len(l)-1]==']');
    int is_test    = (l[0]=='t'&&l[1]=='e'&&l[2]=='s'&&l[3]=='t'&&(l[4]==' '||l[4]==0));
    if (is_bracket || is_test) {
        // Extract inner content
        char inner[SH_LINE_MAX];
        if (is_bracket) {
            int il=str_len(l); str_copy(inner,l+1); inner[il-2]=0;
        } else {
            str_copy(inner, l+5);
        }
        char* s = sh_trim(inner);
        // -f path
        if (s[0]=='-'&&s[1]=='f'&&s[2]==' ') return sys_stat(sh_trim(s+3))>=0?0:1;
        // -d path
        if (s[0]=='-'&&s[1]=='d'&&s[2]==' ') return sys_isdir(sh_trim(s+3))>0?0:1;
        // -e path
        if (s[0]=='-'&&s[1]=='e'&&s[2]==' ') return sys_stat(sh_trim(s+3))>=0?0:1;
        // -z string (zero length)
        if (s[0]=='-'&&s[1]=='z'&&s[2]==' ') { char ex[SH_LINE_MAX];sh_expand(sh_trim(s+3),ex,SH_LINE_MAX);return ex[0]?1:0; }
        // -n string (non-zero length)
        if (s[0]=='-'&&s[1]=='n'&&s[2]==' ') { char ex[SH_LINE_MAX];sh_expand(sh_trim(s+3),ex,SH_LINE_MAX);return ex[0]?0:1; }
        // "A" = "B" or "A" == "B"
        {
            // Find = or !=
            int si=0;
            char lhs[128]; int li=0;
            // strip quotes
            if(s[si]=='"') si++;
            while(s[si]&&s[si]!='"'&&s[si]!=' '&&li<127)lhs[li++]=s[si++];
            lhs[li]=0;
            if(s[si]=='"') si++;
            while(s[si]==' ')si++;
            int neq=0;
            if(s[si]=='!'&&s[si+1]=='='){neq=1;si+=2;}
            else if(s[si]=='='&&s[si+1]=='='){si+=2;}
            else if(s[si]=='='){si++;}
            else return 1;
            while(s[si]==' ')si++;
            char rhs[128]; int ri=0;
            if(s[si]=='"')si++;
            while(s[si]&&s[si]!='"'&&ri<127)rhs[ri++]=s[si++];
            rhs[ri]=0;
            int eq=str_equal(lhs,rhs);
            return neq?!eq:eq?0:1;
        }
    }

    // Run as command and get exit code
    char path[256];
    if (!find_in_path(l, path)) return 1;
    int tty = sys_my_tty(); if(tty<0)tty=0;
    int pid = sys_spawn_tty_args(path, tty, "");
    if (pid < 0) return 1;
    return tox_wait_status(pid);
}

// Find next occurrence of keyword kw at given nesting level starting at ip+1.
// Handles nested if/for/while. Returns line index or -1.
static int sh_find_kw(int ip, const char* open_kw, const char* close_kw) {
    int depth=1;
    for (int i=ip+1;i<sh_nlines;i++) {
        char* l=sh_trim(sh_lines[i]);
        if(sh_kw(l,open_kw)) depth++;
        if(sh_kw(l,close_kw)){depth--;if(!depth)return i;}
        // also count elif/else at depth 1 for if scanning
    }
    return -1;
}

// Find elif/else/fi at same depth starting after ip.
static int sh_find_branch(int ip) {
    int depth=0;
    for(int i=ip+1;i<sh_nlines;i++){
        char* l=sh_trim(sh_lines[i]);
        if(sh_kw(l,"if")||sh_kw(l,"for")||sh_kw(l,"while"))depth++;
        if(sh_kw(l,"fi")||sh_kw(l,"done"))depth--;
        if(depth==0&&(sh_kw(l,"elif")||sh_kw(l,"else")||sh_kw(l,"fi")))return i;
    }
    return -1;
}

// Extract command from "if CMD; then" or "while CMD; do"
static void sh_extract_cond(const char* after_kw, char* out) {
    // skip leading space
    const char* p=after_kw; while(*p==' ')p++;
    int oi=0;
    // copy until '; then', '; do', or end of line
    while(*p&&oi<SH_LINE_MAX-1){
        if(*p==';'){break;}
        out[oi++]=*p++;
    }
    out[oi]=0;
    // strip trailing space
    while(oi>0&&(out[oi-1]==' '||out[oi-1]=='\t'))out[--oi]=0;
}

// Main script executor. Returns last exit code.
static int sh_run(int start, int end) {
    int ip=start;
    while(ip<end){
        char* raw=sh_lines[ip];
        char exp[SH_LINE_MAX]; sh_expand(raw,exp,SH_LINE_MAX);
        char* l=sh_trim(exp);

        if(!l[0]||l[0]=='#'){ip++;continue;}

        // if
        if(sh_kw(l,"if")){
            char cond[SH_LINE_MAX];
            sh_extract_cond(l+2,cond);
            int res=sh_eval_cond(cond);
            int fi_line=sh_find_kw(ip,"if","fi");
            if(fi_line<0){ip++;continue;}
            if(res==0){
                // true: execute body until elif/else/fi
                int branch=sh_find_branch(ip);
                if(branch<0)branch=fi_line;
                sh_run(ip+1,branch);
                ip=fi_line+1;
            } else {
                // false: find elif/else/fi
                int branch=sh_find_branch(ip);
                if(branch<0){ip=fi_line+1;continue;}
                char* bl=sh_trim(sh_lines[branch]);
                if(sh_kw(bl,"else")){
                    // execute else body
                    int fi2=sh_find_kw(branch,"if","fi");
                    if(fi2<0)fi2=fi_line;
                    sh_run(branch+1,fi2);
                    ip=fi2+1;
                } else if(sh_kw(bl,"elif")){
                    // treat elif as a new if at that line
                    // rewrite lines[branch] to "if ..." and recurse
                    char save[SH_LINE_MAX]; str_copy(save,sh_lines[branch]);
                    sh_lines[branch][0]='i'; sh_lines[branch][1]='f';
                    sh_lines[branch][2]=' ';
                    str_copy(sh_lines[branch]+3, sh_trim(save)+4); // skip "elif"
                    ip=branch;
                } else {
                    // fi
                    ip=fi_line+1;
                }
            }
            continue;
        }

        // for VAR in LIST; do ... done
        if(sh_kw(l,"for")){
            char rest[SH_LINE_MAX]; str_copy(rest,l+3);
            char* r=sh_trim(rest);
            // parse VAR
            char var[64]; int vi=0;
            while(*r&&*r!=' '&&vi<63)var[vi++]=*r++;
            var[vi]=0; while(*r==' ')r++;
            // skip "in"
            if(r[0]=='i'&&r[1]=='n'&&r[2]==' ')r+=3;
            // skip to end of list (stop at ';')
            char list[SH_LINE_MAX]; int li=0;
            while(*r&&*r!=';'&&li<SH_LINE_MAX-1)list[li++]=*r++;
            list[li]=0;
            int done_line=sh_find_kw(ip,"for","done");
            if(done_line<0){ip++;continue;}
            // iterate over whitespace-separated tokens
            const char* p=list;
            while(*p){
                while(*p==' ')p++;
                if(!*p)break;
                char item[128]; int ii=0;
                while(*p&&*p!=' '&&ii<127)item[ii++]=*p++;
                item[ii]=0;
                tox_setenv(var,item);
                sh_run(ip+1,done_line);
            }
            ip=done_line+1;
            continue;
        }

        // while COND; do ... done
        if(sh_kw(l,"while")){
            char cond[SH_LINE_MAX];
            sh_extract_cond(l+5,cond);
            int done_line=sh_find_kw(ip,"while","done");
            if(done_line<0){ip++;continue;}
            while(sh_eval_cond(cond)==0)
                sh_run(ip+1,done_line);
            ip=done_line+1;
            continue;
        }

        // fi / done / else / elif — skip (handled by if/for/while above)
        if(sh_kw(l,"fi")||sh_kw(l,"done")||sh_kw(l,"else")||sh_kw(l,"elif")){ip++;continue;}

        // exit [N]
        if(sh_kw(l,"exit")){
            char* rest=sh_trim(l+4);
            sh_exit_code=0;
            while(*rest>='0'&&*rest<='9'){sh_exit_code=sh_exit_code*10+(*rest++-'0');}
            last_exit=sh_exit_code;
            sh_exit_requested=1;
            return sh_exit_code;
        }

        // Plain command
        sh_eval(raw);
        ip++;
        if(sh_exit_requested) return sh_exit_code;
    }
    return 0;
}

static void run_sh_script(const char* path, const char* args_str) {
    int sz = sys_stat(path);
    if (sz <= 0) { set_color(0x0C); print("sh: cannot read: "); print(path); print("\n"); set_color(0x07); return; }

    // Save and set positional parameters
    char save_pos[SH_POS_MAX][128];
    int  save_argc = sh_argc;
    for(int j=0;j<SH_POS_MAX;j++) str_copy(save_pos[j], sh_pos[j]);

    str_copy(sh_pos[0], path);
    sh_argc = 0;
    if (args_str) {
        const char* ap = args_str;
        while (*ap && sh_argc < SH_POS_MAX-1) {
            while (*ap==' ') ap++;
            if (!*ap) break;
            sh_argc++;
            int ai=0;
            while(*ap&&*ap!=' '&&ai<127) sh_pos[sh_argc][ai++]=*ap++;
            sh_pos[sh_argc][ai]=0;
        }
    }

    sh_exit_requested = 0;

    char* buf = malloc((uint32_t)sz+2);
    if (!buf) { set_color(0x0C); print("sh: out of memory\n"); set_color(0x07); return; }
    int fd = sys_open(path, 0x1);
    int n = tox_read(fd, (uint8_t*)buf, (uint32_t)sz);
    tox_close(fd); if(n<0)n=0; buf[n]='\n'; buf[n+1]=0;

    // Split in-place: replace \r\n with \0, store pointers into buf
    sh_nlines=0;
    int i=0;
    if(buf[0]=='#'&&buf[1]=='!'){while(buf[i]&&buf[i]!='\n')i++;i++;} // skip shebang
    sh_lines[sh_nlines=0] = buf+i;
    while(buf[i] && sh_nlines < SH_MAX_LINES-1) {
        if(buf[i]=='\r'){buf[i]=0;i++;continue;}
        if(buf[i]=='\n'){buf[i]=0;i++;sh_lines[++sh_nlines]=buf+i;continue;}
        i++;
    }
    if(buf[i]==0&&sh_nlines<SH_MAX_LINES) sh_nlines++;
    sh_run(0, sh_nlines);
    free(buf);

    // Restore positional parameters
    sh_argc = save_argc;
    for(int j=0;j<SH_POS_MAX;j++) str_copy(sh_pos[j], save_pos[j]);
    sh_exit_requested = 0;
}

// ── Entry point ───────────────────────────────────────────────────────────────

void _start() {
    history_load();
    alias_load();
    tox_setenv("CWD", cwd);
    char input[INPUT_MAX];
    int  len = 0, cur = 0;
    print_prompt();

    while (1) {
        if (!key_available()) { yield(); continue; }
        char c = sys_getchar();
        if (c == 0) continue;

        if (c == KEY_UP || c == KEY_DOWN) {
            int next_pos = history_pos + (c == KEY_UP ? 1 : -1);
            if (next_pos < -1)             next_pos = -1;
            if (next_pos >= history_count) next_pos = history_count-1;
            if (next_pos >= HISTORY_MAX)   next_pos = HISTORY_MAX-1;

            erase_n(cur);
            for (int i=cur;i<len;i++){char b[2]={input[i],0};print(b);}
            erase_n(len-cur);
            len = 0; cur = 0;

            if (next_pos == -1) { history_pos = -1; }
            else {
                const char* e = history_get(next_pos);
                if (e) { history_pos=next_pos; str_copy(input,e); len=str_len(input); cur=len; print(input); }
            }
            continue;
        }

        if (c == KEY_LEFT)  { if (cur > 0)   { cur--; print("\x08"); }       continue; }
        if (c == KEY_RIGHT) { if (cur < len) { print("\x0E"); cur++; }        continue; }
        if (c == '\t')      { do_tab_complete(input, &len, &cur);             continue; }

        if (c == '\n') {
            for (int i=cur;i<len;i++){char b[2]={input[i],0};print(b);}
            print("\n");
            input[len] = 0;
            history_push(input);
            history_append(input);
            history_pos = -1;
            static char exp_cmd[INPUT_MAX * 4];
            sh_expand(input, exp_cmd, sizeof(exp_cmd));
            run_command(exp_cmd);
            len = 0; cur = 0;
            print_prompt();
            continue;
        }

        if (c == 8) {
            if (cur > 0) {
                for (int i=cur-1;i<len-1;i++) input[i]=input[i+1];
                len--; cur--;
                print("\x08");
                for (int i=cur;i<len;i++){char b[2]={input[i],0};print(b);}
                print(" ");
                for (int i=cur;i<len+1;i++) print("\x08");
            }
            continue;
        }

        if (len < INPUT_MAX-1) {
            for (int i=len;i>cur;i--) input[i]=input[i-1];
            input[cur]=c; len++; cur++;
            for (int i=cur-1;i<len;i++){char b[2]={input[i],0};print(b);}
            for (int i=cur;i<len;i++) print("\x08");
        }
    }
}
