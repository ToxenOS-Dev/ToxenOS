// ToxenOS/user/bin/ts.c — .ts script interpreter (C#-inspired)
// Syntax: var/string/int decls, if/else, for, while, foreach, cmd() calls
#include "../tox.h"

// ── helpers ────────────────────────────────────────────────────────────────────
static int  tslen(const char* s)              { return tox_strlen(s); }
static void tscpy(char* d, const char* s)     { tox_strcpy(d, s); }
static int  tseq(const char* a, const char* b){ return !tox_strcmp(a, b); }
static void tscat(char* d, const char* s)     { tox_strcat(d, s); }
static int  tssw(const char* s, const char* p){ return tox_starts_with(s, p); }

static int ts_atoi(const char* s) {
    int neg = 0, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}
static void ts_itoa(int n, char* b) {
    if (n < 0) { b[0] = '-'; ts_itoa(-n, b + 1); return; }
    if (!n)    { b[0] = '0'; b[1] = 0; return; }
    char t[12]; int i = 0;
    while (n) { t[i++] = '0' + n % 10; n /= 10; }
    int j = 0; while (i > 0) b[j++] = t[--i]; b[j] = 0;
}
static void tstrip(char* s) {
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) { int j = 0; while (s[i + j]) { s[j] = s[i + j]; j++; } s[j] = 0; }
    int n = tslen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) n--;
    s[n] = 0;
}

// ── variable table ─────────────────────────────────────────────────────────────
#define VN_MAX 24
#define VK_LEN 32
#define VV_LEN 128
static char vk[VN_MAX][VK_LEN];
static char vv[VN_MAX][VV_LEN];
static int  vn = 0;

static void vs(const char* k, const char* v) {
    for (int i = 0; i < vn; i++) {
        if (tseq(vk[i], k)) {
            int j = 0;
            while (v[j] && j < VV_LEN - 1) { vv[i][j] = v[j]; j++; }
            vv[i][j] = 0; return;
        }
    }
    if (vn >= VN_MAX) return;
    tscpy(vk[vn], k);
    int j = 0;
    while (v[j] && j < VV_LEN - 1) { vv[vn][j] = v[j]; j++; }
    vv[vn][j] = 0;
    vn++;
}
static int vg(const char* k, char* buf, int max) {
    for (int i = 0; i < vn; i++) {
        if (tseq(vk[i], k)) {
            int j = 0;
            while (vv[i][j] && j < max - 1) { buf[j] = vv[i][j]; j++; }
            buf[j] = 0; return 1;
        }
    }
    return tox_getenv(k, buf, max) >= 0;
}

// ── expression evaluator ───────────────────────────────────────────────────────
// Handles: "literal", varname, $varname, number, "str" + var + "str"
static void expand_expr(const char* src, char* out, int max) {
    out[0] = 0; int oi = 0;
    const char* p = src;
    while (*p == ' ') p++;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        if (*p == '"') {
            p++;
            while (*p && *p != '"' && oi < max - 1) {
                if (*p == '$') {
                    p++;
                    char vname[VK_LEN]; int vi = 0;
                    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                           (*p >= '0' && *p <= '9') || *p == '_') {
                        if (vi < VK_LEN - 1) vname[vi++] = *p; p++;
                    }
                    vname[vi] = 0;
                    char val[VV_LEN]; vg(vname, val, VV_LEN);
                    for (int i = 0; val[i] && oi < max - 1; i++) out[oi++] = val[i];
                } else out[oi++] = *p++;
            }
            if (*p == '"') p++;
        } else {
            if (*p == '$') p++;
            char tok[VK_LEN + 4]; int ti = 0;
            while (*p && *p != ' ' && *p != '+' && *p != ')' && *p != ',' &&
                   ti < VK_LEN + 3)
                tok[ti++] = *p++;
            tok[ti] = 0;
            while (ti > 0 && (tok[ti-1] == ' ' || tok[ti-1] == '\t')) tok[--ti] = 0;
            char val[VV_LEN];
            if (!vg(tok, val, VV_LEN)) tscpy(val, tok);
            for (int i = 0; val[i] && oi < max - 1; i++) out[oi++] = val[i];
        }
        out[oi] = 0;
        while (*p == ' ') p++;
        if (*p == '+') p++; else break;
    }
    out[oi] = 0;
}

// ── condition evaluator ────────────────────────────────────────────────────────
static int eval_cond(const char* raw) {
    char buf[128]; tscpy(buf, raw); tstrip(buf);
    const char* ops[] = { "==", "!=", "<=", ">=", "<", ">", 0 };
    int          opl[] = {  2,    2,    2,    2,   1,   1, 0 };

    for (int oi = 0; ops[oi]; oi++) {
        int n = tslen(buf), ol = opl[oi];
        for (int i = 0; i <= n - ol; i++) {
            int m = 1;
            for (int j = 0; j < ol; j++) if (buf[i+j] != ops[oi][j]) { m = 0; break; }
            if (!m) continue;
            if (ol == 1) {
                if (i > 0 && (buf[i-1] == '!' || buf[i-1] == '<' ||
                              buf[i-1] == '>' || buf[i-1] == '=')) continue;
                if (buf[i+1] == '=') continue;
            }
            char lhs[64], rhs[64]; int li = 0, ri = 0;
            for (int k = 0; k < i && li < 63; k++) lhs[li++] = buf[k];
            lhs[li] = 0; tstrip(lhs);
            const char* rs = buf + i + ol; while (*rs == ' ') rs++;
            while (*rs && ri < 63) rhs[ri++] = *rs++; rhs[ri] = 0; tstrip(rhs);

            char lv[128], rv[128];
            expand_expr(lhs, lv, 128); expand_expr(rhs, rv, 128);

            if (tseq(ops[oi], "==")) return  tseq(lv, rv);
            if (tseq(ops[oi], "!=")) return !tseq(lv, rv);
            int li_n = ts_atoi(lv), ri_n = ts_atoi(rv);
            if (tseq(ops[oi], "<"))  return li_n <  ri_n;
            if (tseq(ops[oi], ">"))  return li_n >  ri_n;
            if (tseq(ops[oi], "<=")) return li_n <= ri_n;
            if (tseq(ops[oi], ">=")) return li_n >= ri_n;
        }
    }
    char val[128]; expand_expr(buf, val, 128); return val[0] != 0;
}

// ── block stack ────────────────────────────────────────────────────────────────
#define BLK_IF      1
#define BLK_ELSE    2
#define BLK_FOR     3
#define BLK_WHILE   4
#define BLK_FOREACH 5
#define BLK_MAX     8
#define FE_MAX     16

struct blk {
    int  type, active, open_ip;
    char cond[64];        // for/while condition
    char step[48];        // for step expression
    char var[VK_LEN];     // for variable name
    char fe_var[VK_LEN];  // foreach variable
    char fe_ent[FE_MAX][48];
    int  fe_cnt, fe_idx;
};
static struct blk bs[BLK_MAX];
static int bt   = 0;
static int skip = 0;

// ── glob ───────────────────────────────────────────────────────────────────────
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

static char ts_cwd[256] = "/C:";

static int fe_expand(const char* pat, char ents[][48], int max) {
    char dir[128], gpat[64];
    int sl = -1, n = tslen(pat);
    for (int i = 0; i < n; i++) if (pat[i] == '/') sl = i;
    if (sl < 0) { tscpy(dir, ts_cwd); tscpy(gpat, pat); }
    else {
        int i = 0; for (; i < sl; i++) dir[i] = pat[i]; dir[i] = 0;
        tscpy(gpat, pat + sl + 1);
    }
    char ent[256]; int idx = 0, cnt = 0;
    while (cnt < max && tox_readdir(dir, ent, (uint32_t)idx++) == 0) {
        if (ent[0] == '.') continue;
        if (!glob_match(gpat, ent)) continue;
        if (sl < 0) {
            int i = 0; while (ent[i] && i < 47) ents[cnt][i] = ent[i], i++;
            ents[cnt][i] = 0;
        } else {
            char full[256]; tscpy(full, dir); tscat(full, "/"); tscat(full, ent);
            int i = 0; while (full[i] && i < 47) ents[cnt][i] = full[i], i++;
            ents[cnt][i] = 0;
        }
        cnt++;
    }
    return cnt;
}

// ── path search ────────────────────────────────────────────────────────────────
static int find_cmd(const char* cmd, char* out) {
    if (cmd[0] == '/') {
        tscpy(out, cmd); if (tox_stat(out) >= 0) return 1;
        tscpy(out, cmd); tscat(out, ".nex"); if (tox_stat(out) >= 0) return 1;
        tscpy(out, cmd); tscat(out, ".elf"); if (tox_stat(out) >= 0) return 1;
        return 0;
    }
    char pe[256];
    if (tox_getenv("PATH", pe, sizeof(pe)) < 0)
        tscpy(pe, "/C:/BSM/SystemT:/C:/BSM/usr/lst");
    char dir[128]; const char* pp = pe;
    while (*pp) {
        int dl = 0;
        while (*pp && dl < 127) {
            if (*pp == ';') break;
            if (*pp == ':') {
                int id = (dl == 2 && dir[0] == '/' &&
                    ((dir[1] >= 'A' && dir[1] <= 'Z') || (dir[1] >= 'a' && dir[1] <= 'z'))) ||
                    (dl == 1 && ((dir[0] >= 'A' && dir[0] <= 'Z') || (dir[0] >= 'a' && dir[0] <= 'z')));
                if (id) { dir[dl++] = *pp++; continue; } break;
            }
            dir[dl++] = *pp++;
        }
        dir[dl] = 0; if (*pp == ';' || *pp == ':') pp++; if (!dl) continue;
        tscpy(out, dir); tscat(out, "/"); tscat(out, cmd);
        if (tox_stat(out) >= 0) return 1;
        tscpy(out, dir); tscat(out, "/"); tscat(out, cmd); tscat(out, ".nex");
        if (tox_stat(out) >= 0) return 1;
        tscpy(out, dir); tscat(out, "/"); tscat(out, cmd); tscat(out, ".elf");
        if (tox_stat(out) >= 0) return 1;
    }
    return 0;
}

// ── parse helpers ──────────────────────────────────────────────────────────────
static void extract_paren(const char* line, char* out, int max) {
    const char* p = line; while (*p && *p != '(') p++;
    if (!*p) { out[0] = 0; return; }
    p++; int depth = 1, oi = 0;
    while (*p && depth > 0 && oi < max - 1) {
        if (*p == '(') depth++;
        else if (*p == ')') { depth--; if (!depth) break; }
        if (depth > 0) out[oi++] = *p;
        p++;
    }
    out[oi] = 0; tstrip(out);
}

static void split_for(const char* s, char* ini, char* cond, char* step) {
    int n = tslen(s), s1 = -1, s2 = -1;
    for (int i = 0; i < n; i++)
        if (s[i] == ';') { if (s1 < 0) s1 = i; else if (s2 < 0) { s2 = i; break; } }
    if (s1 < 0) { ini[0] = 0; tscpy(cond, "1"); step[0] = 0; return; }
    int k = 0; for (; k < s1 && k < 63; k++) ini[k] = s[k]; ini[k] = 0; tstrip(ini);
    if (s2 < 0) {
        k = 0; const char* p = s + s1 + 1; while (*p && k < 63) cond[k++] = *p++; cond[k] = 0; tstrip(cond);
        step[0] = 0;
    } else {
        k = 0; const char* p = s + s1 + 1; while (p < s + s2 && k < 63) cond[k++] = *p++; cond[k] = 0; tstrip(cond);
        k = 0; p = s + s2 + 1; while (*p && k < 47) step[k++] = *p++; step[k] = 0; tstrip(step);
    }
}

static void parse_foreach(const char* s, char* fevar, char* fepat) {
    const char* p = s;
    if (tssw(p, "var ")) p += 4; while (*p == ' ') p++;
    int vi = 0; while (*p && *p != ' ' && vi < VK_LEN-1) fevar[vi++] = *p++; fevar[vi] = 0;
    while (*p == ' ') p++;
    if (tssw(p, "in ")) p += 3; while (*p == ' ') p++;
    int pi = 0; while (*p && pi < 63) fepat[pi++] = *p++; fepat[pi] = 0; tstrip(fepat);
}

static int is_else(const char* s) {
    if (s[0] != '}') return 0;
    const char* p = s + 1; while (*p == ' ') p++;
    if (!tssw(p, "else")) return 0;
    p += 4; while (*p == ' ') p++;
    return *p == '{';
}

static int line_opens_block(const char* s) {
    int n = tslen(s); return n > 0 && s[n-1] == '{';
}

// ── assign / increment ─────────────────────────────────────────────────────────
static void exec_assign(const char* s_raw) {
    if (!s_raw || !s_raw[0]) return;
    char s[128]; tscpy(s, s_raw); tstrip(s);

    // Strip type keywords
    if (tssw(s, "var "))    { char t[128]; tscpy(t, s+4);  tscpy(s, t); tstrip(s); }
    else if (tssw(s, "string ")) { char t[128]; tscpy(t, s+7);  tscpy(s, t); tstrip(s); }
    else if (tssw(s, "int "))    { char t[128]; tscpy(t, s+4);  tscpy(s, t); tstrip(s); }

    int n = tslen(s);

    // x++
    if (n >= 3 && s[n-2] == '+' && s[n-1] == '+') {
        s[n-2] = 0; tstrip(s);
        char val[VV_LEN]; if (!vg(s, val, VV_LEN)) tscpy(val, "0");
        int v = ts_atoi(val) + 1; ts_itoa(v, val); vs(s, val); return;
    }
    // x--
    if (n >= 3 && s[n-2] == '-' && s[n-1] == '-') {
        s[n-2] = 0; tstrip(s);
        char val[VV_LEN]; if (!vg(s, val, VV_LEN)) tscpy(val, "0");
        int v = ts_atoi(val) - 1; ts_itoa(v, val); vs(s, val); return;
    }
    // x += expr
    for (int i = 0; i < n - 1; i++) {
        if (s[i] == '+' && s[i+1] == '=') {
            char nm[VK_LEN]; int ni = 0;
            for (int k = 0; k < i && ni < VK_LEN-1; k++) nm[ni++] = s[k]; nm[ni] = 0; tstrip(nm);
            char expr[64]; tscpy(expr, s+i+2); tstrip(expr);
            char oval[VV_LEN]; if (!vg(nm, oval, VV_LEN)) tscpy(oval, "0");
            char ev[VV_LEN]; expand_expr(expr, ev, VV_LEN);
            int r = ts_atoi(oval) + ts_atoi(ev); char res[VV_LEN]; ts_itoa(r, res); vs(nm, res); return;
        }
        if (s[i] == '-' && s[i+1] == '=') {
            char nm[VK_LEN]; int ni = 0;
            for (int k = 0; k < i && ni < VK_LEN-1; k++) nm[ni++] = s[k]; nm[ni] = 0; tstrip(nm);
            char expr[64]; tscpy(expr, s+i+2); tstrip(expr);
            char oval[VV_LEN]; if (!vg(nm, oval, VV_LEN)) tscpy(oval, "0");
            char ev[VV_LEN]; expand_expr(expr, ev, VV_LEN);
            int r = ts_atoi(oval) - ts_atoi(ev); char res[VV_LEN]; ts_itoa(r, res); vs(nm, res); return;
        }
    }
    // x = expr (find first = not part of ==, !=, <=, >=)
    for (int i = 0; i < n; i++) {
        if (s[i] != '=') continue;
        if (i > 0 && (s[i-1] == '!' || s[i-1] == '<' || s[i-1] == '>' || s[i-1] == '=')) continue;
        if (s[i+1] == '=') continue;
        char nm[VK_LEN]; int ni = 0;
        for (int k = 0; k < i && ni < VK_LEN-1; k++) nm[ni++] = s[k]; nm[ni] = 0; tstrip(nm);
        char expr[128]; tscpy(expr, s+i+1); tstrip(expr);
        char val[VV_LEN]; expand_expr(expr, val, VV_LEN);
        vs(nm, val); return;
    }
}

// ── statement executor ─────────────────────────────────────────────────────────
static void exec_stmt(const char* line) {
    // print("...") built-in
    if (tssw(line, "print(")) {
        char arg[256]; int ai = 0;
        const char* p = line + 6; int depth = 1;
        while (*p && depth > 0 && ai < 255) {
            if (*p == '(') depth++;
            else if (*p == ')') { depth--; if (!depth) break; }
            if (depth > 0) arg[ai++] = *p;
            p++;
        }
        arg[ai] = 0;
        char val[256]; expand_expr(arg, val, 256);
        print(val); print("\n"); return;
    }

    // Detect cmd(args) pattern: first ( appears before any =
    int paren = -1, eq = -1;
    for (int i = 0; line[i]; i++) {
        if (line[i] == '(' && paren < 0) { paren = i; break; }
        if (line[i] == '=' && eq < 0)    { eq = i; break; }
    }

    if (paren >= 0 && (eq < 0 || paren < eq)) {
        char name[64]; int ni = 0;
        for (int i = 0; i < paren && ni < 63; i++) name[ni++] = line[i]; name[ni] = 0; tstrip(name);

        // Extract content between outer parens
        const char* inner = line + paren + 1;
        int ilen = tslen(inner), close = -1; int depth = 1;
        for (int i = 0; i < ilen; i++) {
            if (inner[i] == '(') depth++;
            else if (inner[i] == ')') { depth--; if (!depth) { close = i; break; } }
        }
        char argtuple[256]; int ai = 0;
        if (close > 0) { for (int i = 0; i < close && ai < 255; i++) argtuple[ai++] = inner[i]; }
        argtuple[ai] = 0;

        // Build args string from comma-separated expressions
        char args[256]; args[0] = 0; int first = 1;
        const char* p = argtuple;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            char expr[128]; int ei = 0; int d2 = 0;
            while (*p && ei < 127) {
                if (*p == '"') {
                    expr[ei++] = *p++;
                    while (*p && *p != '"' && ei < 127) expr[ei++] = *p++;
                    if (*p == '"') expr[ei++] = *p++;
                } else if (*p == ',' && d2 == 0) break;
                else if (*p == '(') { d2++; expr[ei++] = *p++; }
                else if (*p == ')') { d2--; expr[ei++] = *p++; }
                else expr[ei++] = *p++;
            }
            expr[ei] = 0; tstrip(expr);
            if (ei > 0) {
                char val[128]; expand_expr(expr, val, 128);
                if (!first) tscat(args, " ");
                tscat(args, val); first = 0;
            }
            if (*p == ',') p++;
        }

        // Find and run the command
        char path[128];
        if (!find_cmd(name, path)) {
            set_color(0x0C); print("ts: not found: "); print(name); print("\n"); set_color(0x07);
            return;
        }
        int tty = tox_my_tty(); if (tty < 0) tty = 0;
        int pid = tox_spawn_args(path, tty, args[0] ? args : "");
        if (pid >= 0) { tox_wait(pid); return; }
        // path was already resolved (.nex preferred over .elf) — don't retry
        // with a different extension, just report the exact file that failed.
        set_color(0x0C); print("ts: failed to run: "); print(path); print("\n"); set_color(0x07);
        return;
    }

    // Otherwise: assignment
    exec_assign(line);
}

// ── script lines ───────────────────────────────────────────────────────────────
#define MAX_LINES 256
static char* lines[MAX_LINES];
static int   nlines = 0;
static char  sbuf[8192];

// ── main runner ────────────────────────────────────────────────────────────────
static void run_script(void) {
    int ip = 0;
    skip = 0; bt = 0;
    while (ip < nlines) {
        char* line = lines[ip];

        // Strip // comments
        for (int i = 0; line[i]; i++) {
            if (line[i] == '/' && line[i+1] == '/') { line[i] = 0; break; }
        }
        tstrip(line);

        // Strip trailing semicolons
        int ll = tslen(line);
        while (ll > 0 && line[ll-1] == ';') { line[--ll] = 0; tstrip(line); ll = tslen(line); }

        if (!ll) { ip++; continue; }

        // ── SKIP MODE ──────────────────────────────────────────────────────────
        if (skip > 0) {
            if (is_else(line)) {
                if (skip == 1) {
                    skip = 0;
                    // Transitioning from skipped if-body into else-body
                    if (bt > 0 && bs[bt-1].type == BLK_IF && !bs[bt-1].active) {
                        bt--;
                        if (bt < BLK_MAX) {
                            bs[bt].type = BLK_ELSE; bs[bt].active = 1; bs[bt].open_ip = ip; bt++;
                        }
                    }
                } else skip--;
            } else if (tseq(line, "}")) {
                skip--;
                if (skip == 0 && bt > 0) {
                    // Pop the block whose body we just finished skipping
                    bt--;
                }
            } else if (line_opens_block(line)) skip++;
            ip++; continue;
        }

        // ── EXECUTION MODE ─────────────────────────────────────────────────────

        // }
        if (tseq(line, "}")) {
            if (bt > 0) {
                struct blk* top = &bs[bt-1];
                if (top->type == BLK_FOR) {
                    exec_assign(top->step);
                    if (eval_cond(top->cond)) { ip = top->open_ip + 1; continue; }
                    else bt--;
                } else if (top->type == BLK_WHILE) {
                    if (eval_cond(top->cond)) { ip = top->open_ip + 1; continue; }
                    else bt--;
                } else if (top->type == BLK_FOREACH) {
                    top->fe_idx++;
                    if (top->fe_idx < top->fe_cnt) {
                        vs(top->fe_var, top->fe_ent[top->fe_idx]);
                        ip = top->open_ip + 1; continue;
                    } else bt--;
                } else bt--; // BLK_IF or BLK_ELSE
            }
            ip++; continue;
        }

        // } else {
        if (is_else(line)) {
            if (bt > 0 && bs[bt-1].type == BLK_IF) {
                bt--;
                if (bt < BLK_MAX) {
                    bs[bt].type = BLK_ELSE; bs[bt].active = 0; bs[bt].open_ip = ip; bt++;
                }
                skip = 1;
            }
            ip++; continue;
        }

        // if (cond) {
        if (tssw(line, "if(") || tssw(line, "if (")) {
            char cond[96]; extract_paren(line, cond, sizeof(cond));
            int result = eval_cond(cond);
            if (bt < BLK_MAX) {
                bs[bt].type = BLK_IF; bs[bt].active = result;
                bs[bt].open_ip = ip; bt++;
            }
            if (!result) skip = 1;
            ip++; continue;
        }

        // while (cond) {
        if (tssw(line, "while(") || tssw(line, "while (")) {
            char cond[64]; extract_paren(line, cond, sizeof(cond));
            int result = eval_cond(cond);
            if (bt < BLK_MAX) {
                bs[bt].type = BLK_WHILE; bs[bt].active = result;
                tscpy(bs[bt].cond, cond); bs[bt].open_ip = ip; bt++;
            }
            if (!result) skip = 1;
            ip++; continue;
        }

        // for (init; cond; step) {
        if (tssw(line, "for(") || tssw(line, "for (")) {
            char inner[128]; extract_paren(line, inner, sizeof(inner));
            char ini[64], cond[64], step[48];
            split_for(inner, ini, cond, step);
            exec_assign(ini);
            int result = eval_cond(cond);
            if (bt < BLK_MAX) {
                bs[bt].type = BLK_FOR; bs[bt].active = result;
                tscpy(bs[bt].cond, cond); tscpy(bs[bt].step, step);
                bs[bt].open_ip = ip; bt++;
            }
            if (!result) skip = 1;
            ip++; continue;
        }

        // foreach (var f in pattern) {
        if (tssw(line, "foreach(") || tssw(line, "foreach (")) {
            char inner[128]; extract_paren(line, inner, sizeof(inner));
            char fevar[VK_LEN], fepat[64];
            parse_foreach(inner, fevar, fepat);
            if (bt < BLK_MAX) {
                struct blk* b = &bs[bt];
                b->type = BLK_FOREACH; b->open_ip = ip;
                tscpy(b->fe_var, fevar);
                b->fe_cnt = fe_expand(fepat, b->fe_ent, FE_MAX);
                b->fe_idx = 0;
                b->active = b->fe_cnt > 0;
                bt++;
                if (b->active) vs(fevar, b->fe_ent[0]);
                else           skip = 1;
            }
            ip++; continue;
        }

        // Normal statement
        exec_stmt(line);
        ip++;
    }
}

// ── entry point ────────────────────────────────────────────────────────────────
void _start() {
    static char argbuf[256];
    tox_get_args(argbuf);
    const char* arg = argbuf;
    while (*arg == ' ') arg++;

    if (!arg || !arg[0]) {
        set_color(0x0C); print("Usage: ts <script.ts>\n"); set_color(0x07);
        tox_exit();
    }

    // Load CWD
    if (tox_getenv("CWD", ts_cwd, sizeof(ts_cwd)) < 0)
        tscpy(ts_cwd, "/C:");

    // Resolve path
    static char spath[256];
    if (arg[0] == '/') tscpy(spath, arg);
    else { tscpy(spath, ts_cwd); tscat(spath, "/"); tscat(spath, arg); }

    int sz = tox_stat(spath);
    if (sz < 0) {
        // Try with .ts extension
        tscat(spath, ".ts");
        sz = tox_stat(spath);
    }
    if (sz < 0 || sz >= (int)sizeof(sbuf)) {
        set_color(0x0C); print("ts: not found: "); print(spath); print("\n"); set_color(0x07);
        tox_exit();
    }

    int fd = tox_open(spath, 1);
    if (fd < 0) { set_color(0x0C); print("ts: open failed\n"); set_color(0x07); tox_exit(); }
    int n = tox_read(fd, (uint8_t*)sbuf, (uint32_t)sz);
    tox_close(fd);
    if (n < 0) n = 0; sbuf[n] = 0;

    // Split into lines
    nlines = 0;
    int i = 0;
    if (n > 0) lines[nlines++] = sbuf;
    while (i < n && nlines < MAX_LINES) {
        if (sbuf[i] == '\r' || sbuf[i] == '\n') {
            sbuf[i] = 0;
            i++;  // advance past the null we just wrote
            while (i < n && (sbuf[i] == '\r' || sbuf[i] == '\n')) { sbuf[i] = 0; i++; }
            if (i < n) lines[nlines++] = sbuf + i;
        } else i++;
    }

    run_script();
    tox_exit();
}
