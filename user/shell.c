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


// Arrow key escape codes (sent by keyboard driver)
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x03
#define KEY_RIGHT 0x04



#define INPUT_MAX   256
#define HISTORY_MAX 16

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
}

// ── Single-command spawner ────────────────────────────────────────────────────

// Returns pid, -2 if not found, -1 on spawn error
static int spawn_cmd(const char* cmd, const char* args, int stdin_fd, int stdout_fd) {
    static char path[128];
    if (!find_in_path(cmd, path)) return -2;

    static char full_args[256];
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
        str_copy(full_args, cwd);
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
    for (int s = 0; s < nstages; s++) if (pids[s] >= 0) sys_wait(pids[s]);
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
        return;
    }
    if (str_equal(cmd_buf,"unalias")) {
        if (args && args[0]) alias_remove(args);
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

    run_pipeline(input);
}

// ── Entry point ───────────────────────────────────────────────────────────────

void _start() {
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
            history_pos = -1;
            run_command(input);
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
